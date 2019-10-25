#include "terark_zip_table.h"
#include "terark_zip_common.h"
#include <terark/hash_strmap.hpp>
#include <terark/util/throw.hpp>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include "rocksdb/utilities/write_batch_with_index.h"
#ifdef _MSC_VER
# include <Windows.h>
# define strcasecmp _stricmp
# define strncasecmp _strnicmp
# undef DeleteFile
#else
# include <unistd.h>
#endif
#include <mutex>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

namespace terark {
void DictZipBlobStore_setZipThreads(int zipThreads);
}

namespace rocksdb {

void TerarkZipDeleteTempFiles(const std::string& tmpPath) {
  Env* env = Env::Default();
  std::vector<std::string> files;
  env->GetChildren(tmpPath, &files);
  std::string fpath;
  for (const std::string& f : files) {
    if (false
        || fstring(f).startsWith("Terark-")
        || fstring(f).startsWith("q1-")
        || fstring(f).startsWith("q2-")
        || fstring(f).startsWith("linkSeqVec-")
        || fstring(f).startsWith("linkVec-")
        || fstring(f).startsWith("label-")
        || fstring(f).startsWith("nextStrVec-")
        || fstring(f).startsWith("nestStrVec-")
        || fstring(f).startsWith("nestStrPool-")
        ) {
      fpath.resize(0);
      fpath.append(tmpPath);
      fpath.push_back('/');
      fpath.append(f);
      env->DeleteFile(fpath);
    }
  }
}

// todo(linyuanjin): may use better string impl to replace std::string,
//                   better hash impl to replace std::unordered_map
// config string example: "entropyAlgo=FSE;compaction_style=Universal"
// fields are separated by ';'
// key and value are separated by '='
static std::unordered_map<std::string, std::string>
TerarkGetConfigMapFromEnv() {
  std::unordered_map<std::string, std::string> configMap;
  const char* configCString = getenv("TerarkConfigString");
  if (!configCString || !*configCString) {
    return configMap;
  }

  std::vector<std::string> fields;
  std::string cfgStr(configCString);
  boost::algorithm::split(fields, cfgStr, boost::is_any_of(";\t\r\n"));

  for (const std::string& f : fields) {
    std::string::size_type pos = f.find('=');
    if (pos != std::string::npos) {
      std::string::size_type p = pos + 1;
      configMap[std::string(f.data(), pos)] = std::string(f.data() + p, f.size() - p);
    }
  }
  return configMap;
}

static void TerarkSetEnvFromConfigMapIfNeed(std::unordered_map<std::string, std::string>& configMap) {
    STD_INFO("TerarkSetEnvFromConfigMapIfNeed\n");
    static std::atomic<bool> done{false};
    static std::mutex mtx;
    if (!done.load()){
        std::lock_guard<std::mutex> guard(mtx);
        if (!done.load()){
            for (auto& kv : configMap) {
#ifdef _MSC_VER
                _putenv_s(kv.first.c_str(), kv.second.c_str());
#else
                setenv(kv.first.c_str(), kv.second.c_str(), 1);
#endif
                STD_INFO("%s=%s\n", kv.first.c_str(), kv.second.c_str());
            }
            done.store(true);
        }
    }
}

void TerarkZipAutoConfigForBulkLoad(struct TerarkZipTableOptions& tzo,
                                    struct DBOptions& dbo,
                                    struct ColumnFamilyOptions& cfo,
                                    size_t cpuNum,
                                    size_t memBytesLimit,
                                    size_t diskBytesLimit) {
  using namespace std; // max, min
  int iCpuNum = int(cpuNum);
  if (cpuNum > 0) {
    terark::DictZipBlobStore_setZipThreads(max(iCpuNum - 1, 0));
  }
  if (0 == memBytesLimit) {
#ifdef _MSC_VER
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    GlobalMemoryStatusEx(&statex);
    memBytesLimit = statex.ullTotalPhys;
#else
    size_t page_num  = sysconf(_SC_PHYS_PAGES);
    size_t page_size = sysconf(_SC_PAGE_SIZE);
    memBytesLimit = page_num * page_size;
#endif
  }
  tzo.softZipWorkingMemLimit = memBytesLimit * 7 / 8;
  tzo.hardZipWorkingMemLimit = tzo.softZipWorkingMemLimit;
  tzo.smallTaskMemory = memBytesLimit / 16;
  tzo.indexNestLevel = 3;

  cfo.table_factory = SingleTerarkZipTableFactory(
    tzo, std::shared_ptr<TableFactory>(NewAdaptiveTableFactory()));
  cfo.write_buffer_size = tzo.smallTaskMemory;
  cfo.num_levels = 7;
  cfo.max_write_buffer_number = 16;
  cfo.min_write_buffer_number_to_merge = 1;
  cfo.target_file_size_base = cfo.write_buffer_size;
  cfo.target_file_size_multiplier = 1;
  cfo.compaction_style = rocksdb::kCompactionStyleUniversal;
  cfo.compaction_options_universal.allow_trivial_move = true;

  cfo.max_compaction_bytes = (static_cast<uint64_t>(1) << 60);
  cfo.disable_auto_compactions = true;
  cfo.level0_file_num_compaction_trigger = (1 << 30);
  cfo.level0_slowdown_writes_trigger     = (1 << 30);
  cfo.level0_stop_writes_trigger         = (1 << 30);
  cfo.soft_pending_compaction_bytes_limit = 0;
  cfo.hard_pending_compaction_bytes_limit = 0;

  dbo.create_if_missing = true;
  dbo.allow_concurrent_memtable_write = false;
  dbo.allow_mmap_reads = true;
  dbo.allow_mmap_populate = false;
  dbo.max_background_flushes = 4;
  dbo.max_subcompactions = 1; // no sub compactions
  dbo.new_table_reader_for_compaction_inputs = false;
  dbo.max_open_files = -1;

  dbo.env->SetBackgroundThreads(max(1, min(4, iCpuNum / 2)), rocksdb::Env::HIGH);
}

void TerarkZipAutoConfigForOnlineDB(struct TerarkZipTableOptions& tzo,
                                    struct DBOptions& dbo,
                                    struct ColumnFamilyOptions& cfo,
                                    size_t cpuNum,
                                    size_t memBytesLimit,
                                    size_t diskBytesLimit) {
  TerarkZipAutoConfigForOnlineDB_CFOptions(tzo, cfo, memBytesLimit, diskBytesLimit);
  TerarkZipAutoConfigForOnlineDB_DBOptions(dbo, cpuNum);
}

void
TerarkZipAutoConfigForOnlineDB_DBOptions(struct DBOptions& dbo, size_t cpuNum) {
  using namespace std; // max, min
  int iCpuNum = int(cpuNum);
  if (cpuNum > 0) {
    terark::DictZipBlobStore_setZipThreads((iCpuNum * 3 + 1) / 5);
  }
  dbo.create_if_missing = true;
  dbo.allow_mmap_reads = true;
  dbo.allow_mmap_populate = true;
  dbo.max_background_flushes = 4;
  dbo.max_subcompactions = 4;
  dbo.base_background_compactions = 4;
  dbo.max_background_compactions = 8;
  dbo.allow_concurrent_memtable_write = false;
  dbo.max_background_jobs = dbo.max_background_compactions + dbo.max_background_flushes;

  dbo.env->SetBackgroundThreads(dbo.max_background_compactions, rocksdb::Env::LOW);
  dbo.env->SetBackgroundThreads(dbo.max_background_flushes    , rocksdb::Env::HIGH);
}

void TerarkZipAutoConfigForOnlineDB_CFOptions(struct TerarkZipTableOptions& tzo,
                                              struct ColumnFamilyOptions& cfo,
                                              size_t memBytesLimit,
                                              size_t diskBytesLimit) {
  using namespace std; // max, min
  if (0 == memBytesLimit) {
#ifdef _MSC_VER
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    GlobalMemoryStatusEx(&statex);
    memBytesLimit = statex.ullTotalPhys;
#else
    size_t page_num  = sysconf(_SC_PHYS_PAGES);
    size_t page_size = sysconf(_SC_PAGE_SIZE);
    memBytesLimit = page_num * page_size;
#endif
  }
  tzo.softZipWorkingMemLimit = memBytesLimit * 1 / 8;
  tzo.hardZipWorkingMemLimit = tzo.softZipWorkingMemLimit * 2;
  tzo.smallTaskMemory = memBytesLimit / 64;

  cfo.write_buffer_size = 256ull << 20;
  cfo.num_levels = 7;
  cfo.max_write_buffer_number = 16;
  cfo.target_file_size_base = 128ull << 20;
  cfo.target_file_size_multiplier = 1;
  cfo.compaction_style = rocksdb::kCompactionStyleUniversal;
  cfo.compaction_options_universal.allow_trivial_move = true;

  // intended: less than target_file_size_base
  cfo.max_bytes_for_level_base = cfo.write_buffer_size * 8;
  cfo.max_bytes_for_level_multiplier = 2;

  cfo.level0_file_num_compaction_trigger = 2;
  cfo.level0_slowdown_writes_trigger = 8;
  cfo.level0_stop_writes_trigger = 40;
}

bool TerarkZipConfigFromEnv(DBOptions& dbo, ColumnFamilyOptions& cfo) {
  if (TerarkZipCFOptionsFromEnv(cfo)) {
    TerarkZipDBOptionsFromEnv(dbo);
    return true;
  } else {
    return false;
  }
}

bool TerarkZipCFOptionsFromEnv(ColumnFamilyOptions& cfo, const std::string& terarkTempDirIfNotFound) {
  const char* localTempDir = getenv("TerarkZipTable_localTempDir");
  auto configMap = TerarkGetConfigMapFromEnv();
  if (!configMap.empty()) {
    TerarkSetEnvFromConfigMapIfNeed(configMap);
    auto it = configMap.find("TerarkZipTable_localTempDir");
    if (it == configMap.end() && !terarkTempDirIfNotFound.empty()) {
      TerarkZipDeleteTempFiles(terarkTempDirIfNotFound);
      localTempDir = terarkTempDirIfNotFound.c_str();
    }
    if (it != configMap.end()) {
      localTempDir = it->second.c_str();
    }
  }

  if ((!localTempDir || !*localTempDir) && !terarkTempDirIfNotFound.empty()) {
    localTempDir = terarkTempDirIfNotFound.c_str();
  }
  if (!localTempDir) {
    STD_INFO("TerarkZipConfigFromEnv(dbo, cfo) failed because env TerarkZipTable_localTempDir is not defined\n");
    return false;
  }
  if (!*localTempDir) {
    THROW_STD(invalid_argument,
              "If env TerarkZipTable_localTempDir is defined, it must not be empty");
  }
  struct TerarkZipTableOptions tzo;
  TerarkZipAutoConfigForOnlineDB_CFOptions(tzo, cfo, 0, 0);
  tzo.localTempDir = localTempDir;
  if (const char* algo = getenv("TerarkZipTable_entropyAlgo")) {
    if (strcasecmp(algo, "NoEntropy") == 0) {
      tzo.entropyAlgo = tzo.kNoEntropy;
    } else if (strcasecmp(algo, "FSE") == 0) {
      tzo.entropyAlgo = tzo.kFSE;
    } else if (strcasecmp(algo, "huf") == 0) {
      tzo.entropyAlgo = tzo.kHuffman;
    } else if (strcasecmp(algo, "huff") == 0) {
      tzo.entropyAlgo = tzo.kHuffman;
    } else if (strcasecmp(algo, "huffman") == 0) {
      tzo.entropyAlgo = tzo.kHuffman;
    } else {
      tzo.entropyAlgo = tzo.kNoEntropy;
      STD_WARN(
        "bad env TerarkZipTable_entropyAlgo=%s, must be one of {NoEntropy, FSE, huf}, reset to default 'NoEntropy'\n",
        algo);
    }
  }
  if (const char* env = getenv("TerarkZipTable_indexType")) {
    tzo.indexType = env;
  }

#define MyGetInt(obj, name, Default) \
    obj.name = (int)terark::getEnvLong("TerarkZipTable_" #name, Default)
#define MyGetBool(obj, name, Default) \
    obj.name = terark::getEnvBool("TerarkZipTable_" #name, Default)
#define MyGetDouble(obj, name, Default) \
    obj.name = terark::getEnvDouble("TerarkZipTable_" #name, Default)
#define MyGetXiB(obj, name) \
  if (const char* env = getenv("TerarkZipTable_" #name))\
    obj.name = terark::ParseSizeXiB(env)

#define MyOverrideInt(obj, name) MyGetInt(obj, name, obj.name)

  MyGetInt   (tzo, checksumLevel           , 3    );
  MyGetInt   (tzo, checksumSmallValSize    , 40   );
  MyGetInt   (tzo, indexNestLevel          , 3    );
  MyGetInt   (tzo, terarkZipMinLevel       , 0    );
  MyGetInt   (tzo, debugLevel              , 0    );
  MyGetInt   (tzo, keyPrefixLen            , 0    );
  MyGetInt   (tzo, offsetArrayBlockUnits   , 128  );
  MyGetInt   (tzo, indexNestScale          , 8    );
  if (true
      &&   0 != tzo.offsetArrayBlockUnits
      &&  64 != tzo.offsetArrayBlockUnits
      && 128 != tzo.offsetArrayBlockUnits
    ) {
    STD_WARN(
      "TerarkZipConfigFromEnv: bad offsetArrayBlockUnits = %d, must be one of {0,64,128}, reset to 128\n",
      tzo.offsetArrayBlockUnits
    );
    tzo.offsetArrayBlockUnits = 128;
  }
  if (tzo.indexNestScale == 0) {
    STD_WARN(
      "TerarkZipConfigFromEnv: bad indexNestScale = %d, must be in [1,255], reset to 8\n", int(tzo.indexNestScale)
    );
    tzo.indexNestScale = 8;
  }

  MyGetBool  (tzo, useSuffixArrayLocalMatch, false);
  MyGetBool  (tzo, warmUpIndexOnOpen       , true );
  MyGetBool  (tzo, warmUpValueOnOpen       , false);
  MyGetBool  (tzo, disableSecondPassIter   , false);
  MyGetBool  (tzo, enableCompressionProbe  , true );
  MyGetBool  (tzo, disableCompressDict     , false);
  MyGetBool  (tzo, optimizeCpuL3Cache      , true );
  MyGetBool  (tzo, forceMetaInMemory       , false);
  MyGetBool  (tzo, enableEntropyStore      , true );

  MyGetDouble(tzo, sampleRatio             , 0.03 );
  MyGetDouble(tzo, indexCacheRatio         , 0.00 );

  MyGetInt(tzo, minDictZipValueSize, 15);
  MyGetInt(tzo, minPreadLen, 0);

  MyGetXiB(tzo, softZipWorkingMemLimit);
  MyGetXiB(tzo, hardZipWorkingMemLimit);
  MyGetXiB(tzo, smallTaskMemory);
  MyGetXiB(tzo, singleIndexMinSize);
  MyGetXiB(tzo, singleIndexMaxSize);
  MyGetXiB(tzo, cacheCapacityBytes);
  MyGetInt(tzo, cacheShards, 67);

  tzo.singleIndexMinSize = std::max<size_t>(tzo.singleIndexMinSize, 1ull << 20);
  tzo.singleIndexMaxSize = std::min<size_t>(tzo.singleIndexMaxSize, 0x1E0000000);

  cfo.table_factory = SingleTerarkZipTableFactory(
    tzo, std::shared_ptr<TableFactory>(NewAdaptiveTableFactory()));
  const char* compaction_style = "Universal";
  if (const char* env = getenv("TerarkZipTable_compaction_style")) {
    compaction_style = env;
  }
  if (strcasecmp(compaction_style, "Level") == 0) {
    cfo.compaction_style = kCompactionStyleLevel;
  } else {
    if (strcasecmp(compaction_style, "Universal") != 0) {
      STD_WARN(
        "bad env TerarkZipTable_compaction_style=%s, use default 'universal'",
        compaction_style);
    }
    cfo.compaction_style = kCompactionStyleUniversal;
    cfo.compaction_options_universal.allow_trivial_move = true;
#define MyGetUniversal_uint(name, Default)  \
    cfo.compaction_options_universal.name = \
        terark::getEnvLong("TerarkZipTable_" #name, Default)
    MyGetUniversal_uint(min_merge_width, 3);
    MyGetUniversal_uint(max_merge_width, 7);
    cfo.compaction_options_universal.size_ratio = (unsigned)
      terark::getEnvLong("TerarkZipTable_universal_compaction_size_ratio",
                         cfo.compaction_options_universal.size_ratio);
    const char* env_stop_style =
      getenv("TerarkZipTable_universal_compaction_stop_style");
    if (env_stop_style) {
      auto& ou = cfo.compaction_options_universal;
      if (strncasecmp(env_stop_style, "Similar", 7) == 0) {
        ou.stop_style = kCompactionStopStyleSimilarSize;
      } else if (strncasecmp(env_stop_style, "Total", 5) == 0) {
        ou.stop_style = kCompactionStopStyleTotalSize;
      } else {
        STD_WARN(
          "bad env TerarkZipTable_universal_compaction_stop_style=%s, use rocksdb default 'TotalSize'",
          env_stop_style);
        ou.stop_style = kCompactionStopStyleTotalSize;
      }
    }
  }
  MyGetXiB(cfo, write_buffer_size);
  MyGetXiB(cfo, target_file_size_base);
  MyGetBool(cfo, enable_lazy_compaction, true);
  MyOverrideInt(cfo, max_write_buffer_number);
  MyOverrideInt(cfo, target_file_size_multiplier);
  MyOverrideInt(cfo, num_levels);
  MyOverrideInt(cfo, level0_file_num_compaction_trigger);
  MyOverrideInt(cfo, level0_slowdown_writes_trigger);
  MyOverrideInt(cfo, level0_stop_writes_trigger);

  cfo.max_compaction_bytes = cfo.target_file_size_base;
  MyGetXiB(cfo, max_compaction_bytes);

  if (tzo.debugLevel) {
    STD_INFO("TerarkZipConfigFromEnv(dbo, cfo) successed\n");
  }
  return true;
}

void TerarkZipDBOptionsFromEnv(DBOptions& dbo) {
  auto configMap = TerarkGetConfigMapFromEnv();
  if (!configMap.empty()) {
    TerarkSetEnvFromConfigMapIfNeed(configMap);
  }

  TerarkZipAutoConfigForOnlineDB_DBOptions(dbo, 0);

  MyGetInt(dbo, base_background_compactions, 4);
  MyGetInt(dbo,  max_background_compactions, 8);
  MyGetInt(dbo,  max_background_flushes    , 4);
  MyGetInt(dbo,  max_subcompactions        , 4);
  MyGetBool(dbo, allow_mmap_populate       , false);
  dbo.max_background_jobs = dbo.max_background_flushes + dbo.max_background_compactions;

  dbo.env->SetBackgroundThreads(dbo.max_background_compactions, rocksdb::Env::LOW);
  dbo.env->SetBackgroundThreads(dbo.max_background_flushes    , rocksdb::Env::HIGH);
  dbo.allow_mmap_reads = true;
  dbo.new_table_reader_for_compaction_inputs = false;
  dbo.max_open_files = -1;
}

class TerarkBlackListCF : public terark::hash_strmap<> {
public:
  TerarkBlackListCF() {
    if (const char* env = getenv("TerarkZipTable_blackListColumnFamily")) {
      const char* end = env + strlen(env);
      for (auto curr = env; curr < end;) {
        auto next = std::find(curr, end, ',');
        this->insert_i(fstring(curr, next));
        curr = next + 1;
      }
    }
  }
};
static TerarkBlackListCF g_blacklistCF;

bool TerarkZipIsBlackListCF(const std::string& cfname) {
  return g_blacklistCF.exists(cfname);
}

template<class T>
T& auto_const_cast(const T& x) {
  return const_cast<T&>(x);
}

void
TerarkZipMultiCFOptionsFromEnv(const DBOptions& db_options,
                               const std::vector<ColumnFamilyDescriptor>& cfvec,
                               const std::string& terarkTempDirIfNotFound) {
  size_t numIsBlackList = 0;
  for (auto& cf : auto_const_cast(cfvec)) {
    if (TerarkZipIsBlackListCF(cf.name)) {
      numIsBlackList++;
    } else {
      TerarkZipCFOptionsFromEnv(cf.options, terarkTempDirIfNotFound);
    }
  }
  if (numIsBlackList < cfvec.size()) {
    TerarkZipDBOptionsFromEnv(auto_const_cast(db_options));
  }
}

ROCKSDB_REGISTER_MEM_TABLE("patricia", PatriciaTrieRepFactory);
ROCKSDB_REGISTER_WRITE_BATCH_WITH_INDEX(patricia);

}