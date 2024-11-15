/* Create main thread instances
   Copyright (C) 2018-2024 Adam Leszczynski (aleszczynski@bersler.com)

This file is part of OpenLogReplicator.

OpenLogReplicator is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as published
by the Free Software Foundation; either version 3, or (at your option)
any later version.

OpenLogReplicator is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenLogReplicator; see the file LICENSE;  If not see
<http://www.gnu.org/licenses/>.  */

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <regex>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "builder/BuilderJson.h"
#include "common/Ctx.h"
#include "common/MemoryManager.h"
#include "common/Thread.h"
#include "common/types.h"
#include "common/types.h"
#include "common/typeTime.h"
#include "common/exception/ConfigurationException.h"
#include "common/exception/RuntimeException.h"
#include "common/metrics/Metrics.h"
#include "common/table/SysObj.h"
#include "common/table/SysUser.h"
#include "locales/Locales.h"
#include "metadata/Checkpoint.h"
#include "metadata/Metadata.h"
#include "metadata/SchemaElement.h"
#include "metadata/SerializerJson.h"
#include "parser/TransactionBuffer.h"
#include "replicator/Replicator.h"
#include "replicator/ReplicatorBatch.h"
#include "state/StateDisk.h"
#include "writer/WriterDiscard.h"
#include "writer/WriterFile.h"
#include "OpenLogReplicator.h"

#ifdef LINK_LIBRARY_OCI
#include "replicator/ReplicatorOnline.h"
#endif /* LINK_LIBRARY_OCI */

#ifdef LINK_LIBRARY_PROTOBUF
#include "builder/BuilderProtobuf.h"
#include "stream/StreamNetwork.h"
#include "writer/WriterStream.h"
#ifdef LINK_LIBRARY_ZEROMQ
#include "stream/StreamZeroMQ.h"
#endif /* LINK_LIBRARY_ZEROMQ */
#endif /* LINK_LIBRARY_PROTOBUF */

#ifdef LINK_LIBRARY_RDKAFKA
#include "writer/WriterKafka.h"
#endif /* LINK_LIBRARY_RDKAFKA */

#ifdef LINK_LIBRARY_PROMETHEUS
#include "common/metrics/MetricsPrometheus.h"
#endif /* LINK_LIBRARY_PROMETHEUS */

namespace OpenLogReplicator {
    OpenLogReplicator::OpenLogReplicator(const std::string& newConfigFileName, Ctx* newCtx) :
            replicator(nullptr),
            fid(-1),
            configFileBuffer(nullptr),
            configFileName(newConfigFileName),
            ctx(newCtx) {
        typeIntX::initializeBASE10();
    }

    OpenLogReplicator::~OpenLogReplicator() {
        if (replicator != nullptr)
            replicators.push_back(replicator);

        ctx->stopSoft();
        ctx->mainFinish();

        for (Writer* writer: writers)
            delete writer;
        writers.clear();

        for (Builder* builder: builders)
            delete builder;
        builders.clear();

        for (Replicator* replicatorTmp: replicators)
            delete replicatorTmp;
        replicators.clear();

        for (Checkpoint* checkpoint: checkpoints)
            delete checkpoint;
        checkpoints.clear();

        for (TransactionBuffer* transactionBuffer: transactionBuffers)
            delete transactionBuffer;
        transactionBuffers.clear();

        for (Metadata* metadata: metadatas)
            delete metadata;
        metadatas.clear();

        for (Locales* locales: localess)
            delete locales;
        localess.clear();

        for (MemoryManager* memoryManager: memoryManagers)
            delete memoryManager;
        memoryManagers.clear();

        if (fid != -1)
            close(fid);
        if (configFileBuffer != nullptr)
            delete[] configFileBuffer;
        configFileBuffer = nullptr;
    }

    int OpenLogReplicator::run() {
        auto locales = new Locales();
        localess.push_back(locales);
        locales->initialize();

        if (unlikely(ctx->trace & Ctx::TRACE::THREADS)) {
            std::ostringstream ss;
            ss << std::this_thread::get_id();
            ctx->logTrace(Ctx::TRACE::THREADS, "main (" + ss.str() + ") start");
        }

        struct stat configFileStat;
        fid = open(configFileName.c_str(), O_RDONLY);
        if (fid == -1)
            throw RuntimeException(10001, "file: " + configFileName + " - open for read returned: " + strerror(errno));

        if (flock(fid, LOCK_EX | LOCK_NB))
            throw RuntimeException(10002, "file: " + configFileName + " - lock operation returned: " + strerror(errno));

        if (stat(configFileName.c_str(), &configFileStat) != 0)
            throw RuntimeException(10003, "file: " + configFileName + " - get metadata returned: " + strerror(errno));

        if (configFileStat.st_size > Checkpoint::CONFIG_FILE_MAX_SIZE || configFileStat.st_size == 0)
            throw ConfigurationException(10004, "file: " + configFileName + " - wrong size: " + std::to_string(configFileStat.st_size));

        configFileBuffer = new char[configFileStat.st_size + 1];
        uint64_t bytesRead = read(fid, configFileBuffer, configFileStat.st_size);
        if (bytesRead != static_cast<uint64_t>(configFileStat.st_size))
            throw RuntimeException(10005, "file: " + configFileName + " - " + std::to_string(bytesRead) + " bytes read instead of " +
                                          std::to_string(configFileStat.st_size));
        configFileBuffer[configFileStat.st_size] = 0;

        rapidjson::Document document;
        if (document.Parse(configFileBuffer).HasParseError())
            throw DataException(20001, "file: " + configFileName + " offset: " + std::to_string(document.GetErrorOffset()) +
                                       " - parse error: " + GetParseError_En(document.GetParseError()));

        if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
            static const char* documentNames[]{"version", "dump-path", "dump-raw-data", "dump-redo-log", "log-level", "trace",
                                               "source", "target", nullptr};
            Ctx::checkJsonFields(configFileName, document, documentNames);
        }

        const char* version = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, document, "version");
        if (strcmp(version, OpenLogReplicator_SCHEMA_VERSION) != 0)
            throw ConfigurationException(30001, "bad JSON, invalid \"version\" value: " + std::string(version) + ", expected: " +
                                                OpenLogReplicator_SCHEMA_VERSION);

        if (document.HasMember("dump-redo-log")) {
            ctx->dumpRedoLog = Ctx::getJsonFieldU64(configFileName, document, "dump-redo-log");
            if (ctx->dumpRedoLog > 2)
                throw ConfigurationException(30001, "bad JSON, invalid \"dump-redo-log\" value: " + std::to_string(ctx->dumpRedoLog) +
                                                    ", expected: one of {0 .. 2}");

            if (ctx->dumpRedoLog > 0) {
                if (document.HasMember("dump-path"))
                    ctx->dumpPath = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, document, "dump-path");

                if (document.HasMember("dump-raw-data")) {
                    ctx->dumpRawData = Ctx::getJsonFieldU64(configFileName, document, "dump-raw-data");
                    if (ctx->dumpRawData > 1)
                        throw ConfigurationException(30001, "bad JSON, invalid \"dump-raw-data\" value: " +
                                                            std::to_string(ctx->dumpRawData) + ", expected: one of {0, 1}");
                }
            }
        }

        if (document.HasMember("log-level")) {
            ctx->logLevel = Ctx::getJsonFieldU64(configFileName, document, "log-level");
            if (ctx->logLevel > Ctx::LOG::DEBUG)
                throw ConfigurationException(30001, "bad JSON, invalid \"log-level\" value: " + std::to_string(ctx->logLevel) +
                                                    ", expected: one of {0 .. 4}");
        }

        if (document.HasMember("trace")) {
            ctx->trace = Ctx::getJsonFieldU64(configFileName, document, "trace");
            if (ctx->trace > 524287)
                throw ConfigurationException(30001, "bad JSON, invalid \"trace\" value: " + std::to_string(ctx->trace) +
                                                    ", expected: one of {0 .. 524287}");
        }

        // Iterate through sources
        const rapidjson::Value& sourceArrayJson = Ctx::getJsonFieldA(configFileName, document, "source");
        if (sourceArrayJson.Size() != 1) {
            throw ConfigurationException(30001, "bad JSON, invalid \"source\" value: " + std::to_string(sourceArrayJson.Size()) +
                                                " elements, expected: 1 element");
        }

        for (rapidjson::SizeType j = 0; j < sourceArrayJson.Size(); ++j) {
            const rapidjson::Value& sourceJson = Ctx::getJsonFieldO(configFileName, sourceArrayJson, "source", j);

            if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                static const char* sourceNames[]{"alias", "memory", "name", "reader", "flags", "state", "debug",
                                                 "transaction-max-mb", "metrics", "format", "redo-read-sleep-us", "arch-read-sleep-us",
                                                 "arch-read-tries", "redo-verify-delay-us", "refresh-interval-us", "arch",
                                                 "filter", nullptr};
                Ctx::checkJsonFields(configFileName, sourceJson, sourceNames);
            }

            const char* alias = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, sourceJson, "alias");
            ctx->info(0, "adding source: " + std::string(alias));

            uint64_t memoryMinMb = 32;
            uint64_t memoryMaxMb = 2048;
            uint64_t memoryReadBufferMaxMb = 128;
            uint64_t memoryReadBufferMinMb = 4;
            uint64_t memorySwapMb = memoryMaxMb * 3 / 4;
            const char* memorySwapPath{"."};
            uint64_t memoryUnswapBufferMinMb = 4;
            uint64_t memoryWriteBufferMaxMb = memoryMaxMb;
            uint64_t memoryWriteBufferMinMb = 4;

            // MEMORY
            if (sourceJson.HasMember("memory")) {
                const rapidjson::Value& memoryJson = Ctx::getJsonFieldO(configFileName, sourceJson, "memory");

                if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                    static const char* memoryNames[]{"min-mb", "max-mb", "read-buffer-max-mb", "read-buffer-min-mb", "swap-mb", "swap-path",
                                                     "unswap-buffer-min-mb", "write-buffer-max-mb", "write-buffer-min-mb", nullptr};
                    Ctx::checkJsonFields(configFileName, memoryJson, memoryNames);
                }

                if (memoryJson.HasMember("min-mb")) {
                    memoryMinMb = Ctx::getJsonFieldU64(configFileName, memoryJson, "min-mb");
                    memoryMinMb = (memoryMinMb / Ctx::MEMORY_CHUNK_SIZE_MB) * Ctx::MEMORY_CHUNK_SIZE_MB;
                    if (memoryMinMb < Ctx::MEMORY_CHUNK_MIN_MB)
                        throw ConfigurationException(30001, "bad JSON, invalid \"min-mb\" value: " + std::to_string(memoryMinMb) +
                                                            ", expected: at least " + std::to_string(Ctx::MEMORY_CHUNK_MIN_MB));
                }

                if (memoryJson.HasMember("max-mb")) {
                    memoryMaxMb = Ctx::getJsonFieldU64(configFileName, memoryJson, "max-mb");
                    memoryMaxMb = (memoryMaxMb / Ctx::MEMORY_CHUNK_SIZE_MB) * Ctx::MEMORY_CHUNK_SIZE_MB;
                    if (memoryMaxMb < memoryMinMb)
                        throw ConfigurationException(30001, "bad JSON, invalid \"max-mb\" value: " + std::to_string(memoryMaxMb) +
                                                            ", expected: at least like \"min-mb\" value (" + std::to_string(memoryMinMb) + ")");

                    memoryReadBufferMaxMb = memoryMaxMb / 8;
                    if (memoryReadBufferMaxMb > 128)
                        memoryReadBufferMaxMb = 128;
                    memoryWriteBufferMaxMb = memoryMaxMb;
                    if (memoryWriteBufferMaxMb > 2048)
                        memoryWriteBufferMaxMb = 2048;
                    memorySwapMb = memoryMaxMb * 3 / 4;
                }

                if (memoryJson.HasMember("unswap-buffer-min-mb")) {
                    memoryUnswapBufferMinMb = Ctx::getJsonFieldU64(configFileName, memoryJson, "unswap-buffer-min-mb");
                    memoryUnswapBufferMinMb = (memoryUnswapBufferMinMb / Ctx::MEMORY_CHUNK_SIZE_MB) * Ctx::MEMORY_CHUNK_SIZE_MB;
                }

                if (memoryJson.HasMember("swap-mb")) {
                    memorySwapMb = Ctx::getJsonFieldU64(configFileName, memoryJson, "swap-mb");
                    memorySwapMb = (memorySwapMb / Ctx::MEMORY_CHUNK_SIZE_MB) * Ctx::MEMORY_CHUNK_SIZE_MB;
                    if (memorySwapMb > memoryMaxMb - 4)
                        throw ConfigurationException(30001, "bad JSON, invalid \"swap-mb\" value: " + std::to_string(memorySwapMb) +
                                                            ", expected maximum \"max-mb\"-1 value (" + std::to_string(memoryMaxMb - 4) + ")");
                }

                if (memoryJson.HasMember("read-buffer-min-mb")) {
                    memoryReadBufferMinMb = Ctx::getJsonFieldU64(configFileName, memoryJson, "read-buffer-min-mb");
                    memoryReadBufferMinMb = (memoryReadBufferMinMb / Ctx::MEMORY_CHUNK_SIZE_MB) * Ctx::MEMORY_CHUNK_SIZE_MB;
                    if (memoryReadBufferMinMb > memoryMaxMb)
                        throw ConfigurationException(30001, "bad JSON, invalid \"read-buffer-min-mb\" value: " +
                                                            std::to_string(memoryReadBufferMaxMb) +
                                                            ", expected: not greater than \"max-mb\" value (" + std::to_string(memoryMaxMb) + ")");
                    if (memoryReadBufferMinMb < 4)
                        throw ConfigurationException(30001, "bad JSON, invalid \"read-buffer-min-mb\" value: " +
                                                            std::to_string(memoryReadBufferMaxMb) + ", expected: at least: 4");
                }

                if (memoryJson.HasMember("read-buffer-max-mb")) {
                    memoryReadBufferMaxMb = Ctx::getJsonFieldU64(configFileName, memoryJson, "read-buffer-max-mb");
                    memoryReadBufferMaxMb = (memoryReadBufferMaxMb / Ctx::MEMORY_CHUNK_SIZE_MB) * Ctx::MEMORY_CHUNK_SIZE_MB;
                    if (memoryReadBufferMaxMb > memoryMaxMb)
                        throw ConfigurationException(30001, "bad JSON, invalid \"read-buffer-max-mb\" value: " +
                                                            std::to_string(memoryReadBufferMaxMb) +
                                                            ", expected: not greater than \"max-mb\" value (" + std::to_string(memoryMaxMb) + ")");
                    if (memoryReadBufferMaxMb < memoryReadBufferMinMb)
                        throw ConfigurationException(30001, "bad JSON, invalid \"read-buffer-max-mb\" value: " +
                                                            std::to_string(memoryReadBufferMaxMb) + ", expected: at least: \"read-buffer-min-mb\" value (" +
                                                            std::to_string(memoryReadBufferMinMb) + ")");
                }

                if (memoryJson.HasMember("write-buffer-min-mb")) {
                    memoryWriteBufferMinMb = Ctx::getJsonFieldU64(configFileName, memoryJson, "write-buffer-min-mb");
                    memoryWriteBufferMinMb = (memoryWriteBufferMinMb / Ctx::MEMORY_CHUNK_SIZE_MB) * Ctx::MEMORY_CHUNK_SIZE_MB;
                    if (memoryWriteBufferMinMb > memoryMaxMb)
                        throw ConfigurationException(30001, "bad JSON, invalid \"write-buffer-min-mb\" value: " +
                                                            std::to_string(memoryWriteBufferMinMb) +
                                                            ", expected: not greater than \"max-mb\" value (" + std::to_string(memoryMaxMb) + ")");
                    if (memoryWriteBufferMinMb < 4)
                        throw ConfigurationException(30001, "bad JSON, invalid \"write-buffer-min-mb\" value: " +
                                                            std::to_string(memoryWriteBufferMinMb) + ", expected: at least: 4");
                }

                if (memoryJson.HasMember("write-buffer-max-mb")) {
                    memoryWriteBufferMaxMb = Ctx::getJsonFieldU64(configFileName, memoryJson, "write-buffer-max-mb");
                    memoryWriteBufferMaxMb = (memoryWriteBufferMaxMb / Ctx::MEMORY_CHUNK_SIZE_MB) * Ctx::MEMORY_CHUNK_SIZE_MB;
                    if (memoryWriteBufferMaxMb > memoryMaxMb)
                        throw ConfigurationException(30001, "bad JSON, invalid \"write-buffer-max-mb\" value: " +
                                                            std::to_string(memoryWriteBufferMaxMb) +
                                                            ", expected: not greater than \"max-mb\" value (" + std::to_string(memoryMaxMb) + ")");
                    if (memoryWriteBufferMaxMb < memoryWriteBufferMinMb)
                        throw ConfigurationException(30001, "bad JSON, invalid \"write-buffer-max-mb\" value: " +
                                                            std::to_string(memoryWriteBufferMaxMb) + ", expected: at least: \"write-buffer-min-mb\" value (" +
                                                            std::to_string(memoryWriteBufferMinMb) + ")");
                }

                if (memoryJson.HasMember("swap-path") && memorySwapMb > 0)
                    memorySwapPath = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, memoryJson, "swap-path");

                if (memoryUnswapBufferMinMb + memoryReadBufferMinMb + memoryWriteBufferMinMb + 4 > memoryMaxMb)
                    throw ConfigurationException(30001, "bad JSON, invalid \"unswap-buffer-min-mb\" + \"read-buffer-min-mb\" + \"write-buffer-min-mb\" + 4 (" +
                                                        std::to_string(memoryUnswapBufferMinMb) + " + " + std::to_string(memoryReadBufferMinMb) +
                                                        " + " + std::to_string(memoryWriteBufferMinMb) + " + 4) is greater than \"max-mb\" value (" +
                                                        std::to_string(memoryMaxMb) + ")");
            }

            const char* name = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, sourceJson, "name");
            const rapidjson::Value& readerJson = Ctx::getJsonFieldO(configFileName, sourceJson, "reader");

            if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                static const char* readerNames[]{"disable-checks", "start-scn", "start-seq", "start-time-rel", "start-time",
                                                 "con-id", "type", "redo-copy-path", "db-timezone", "host-timezone", "log-timezone",
                                                 "user", "password", "server", "redo-log", "path-mapping", "log-archive-format",
                                                 nullptr};
                Ctx::checkJsonFields(configFileName, readerJson, readerNames);
            }

            if (sourceJson.HasMember("flags")) {
                ctx->flags = Ctx::getJsonFieldU64(configFileName, sourceJson, "flags");
                if (ctx->flags > 524287)
                    throw ConfigurationException(30001, "bad JSON, invalid \"flags\" value: " + std::to_string(ctx->flags) +
                                                        ", expected: one of {0 .. 524287}");
                if (ctx->isFlagSet(Ctx::REDO_FLAGS::DIRECT_DISABLE))
                    ctx->redoVerifyDelayUs = 500000;
            }

            if (readerJson.HasMember("disable-checks")) {
                ctx->disableChecks = Ctx::getJsonFieldU64(configFileName, readerJson, "disable-checks");
                if (ctx->disableChecks > 15)
                    throw ConfigurationException(30001, "bad JSON, invalid \"disable-checks\" value: " +
                                                        std::to_string(ctx->disableChecks) + ", expected: one of {0 .. 15}");
            }

            typeScn startScn = Ctx::ZERO_SCN;
            if (readerJson.HasMember("start-scn"))
                startScn = Ctx::getJsonFieldU64(configFileName, readerJson, "start-scn");

            typeSeq startSequence = Ctx::ZERO_SEQ;
            if (readerJson.HasMember("start-seq"))
                startSequence = Ctx::getJsonFieldU32(configFileName, readerJson, "start-seq");

            uint64_t startTimeRel = 0;
            if (readerJson.HasMember("start-time-rel")) {
                startTimeRel = Ctx::getJsonFieldU64(configFileName, readerJson, "start-time-rel");
                if (startScn != Ctx::ZERO_SCN)
                    throw ConfigurationException(30001, "bad JSON, invalid \"start-time-rel\" value: " + std::to_string(startTimeRel) +
                                                        ", expected: unset when \"start-scn\" is set (" + std::to_string(startScn) + ")");
            }

            const char* startTime = "";
            if (readerJson.HasMember("start-time")) {
                startTime = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, readerJson, "start-time");
                if (startScn != Ctx::ZERO_SCN)
                    throw ConfigurationException(30001, "bad JSON, invalid \"start-time\" value: " + std::string(startTime) +
                                                        ", expected: unset when \"start-scn\" is set (" + std::to_string(startScn) + ")");
                if (startTimeRel > 0)
                    throw ConfigurationException(30001, "bad JSON, invalid \"start-time\" value: " + std::string(startTime) +
                                                        ", expected: unset when \"start-time-rel\" is set (" + std::to_string(startTimeRel) + ")");
            }

            uint64_t stateType = State::TYPE_DISK;
            const char* statePath = "checkpoint";

            if (sourceJson.HasMember("state")) {
                const rapidjson::Value& stateJson = Ctx::getJsonFieldO(configFileName, sourceJson, "state");

                if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                    static const char* stateNames[]{"type", "path", "interval-s", "interval-mb", "keep-checkpoints",
                                                    "schema-force-interval", nullptr};
                    Ctx::checkJsonFields(configFileName, stateJson, stateNames);
                }

                if (stateJson.HasMember("type")) {
                    const char* stateTypeStr = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, stateJson, "type");
                    if (strcmp(stateTypeStr, "disk") == 0) {
                        stateType = State::TYPE_DISK;
                        if (stateJson.HasMember("path"))
                            statePath = Ctx::getJsonFieldS(configFileName, Ctx::MAX_PATH_LENGTH, stateJson, "path");
                    } else
                        throw ConfigurationException(30001, std::string("bad JSON, invalid \"type\" value: ") + stateTypeStr +
                                                            ", expected: one of {\"disk\"}");
                }

                if (stateJson.HasMember("interval-s"))
                    ctx->checkpointIntervalS = Ctx::getJsonFieldU64(configFileName, stateJson, "interval-s");

                if (stateJson.HasMember("interval-mb"))
                    ctx->checkpointIntervalMb = Ctx::getJsonFieldU64(configFileName, stateJson, "interval-mb");

                if (stateJson.HasMember("keep-checkpoints"))
                    ctx->checkpointKeep = Ctx::getJsonFieldU64(configFileName, stateJson, "keep-checkpoints");

                if (stateJson.HasMember("schema-force-interval"))
                    ctx->schemaForceInterval = Ctx::getJsonFieldU64(configFileName, stateJson, "schema-force-interval");
            }

            const char* debugOwner = nullptr;
            const char* debugTable = nullptr;

            if (sourceJson.HasMember("debug")) {
                const rapidjson::Value& debugJson = Ctx::getJsonFieldO(configFileName, sourceJson, "debug");

                if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                    static const char* debugNames[]{"stop-log-switches", "stop-checkpoints", "stop-transactions", "owner", "table",
                                                    nullptr};
                    Ctx::checkJsonFields(configFileName, debugJson, debugNames);
                }

                if (debugJson.HasMember("stop-log-switches")) {
                    ctx->stopLogSwitches = Ctx::getJsonFieldU64(configFileName, debugJson, "stop-log-switches");
                    ctx->info(0, "will shutdown after " + std::to_string(ctx->stopLogSwitches) + " log switches");
                }

                if (debugJson.HasMember("stop-checkpoints")) {
                    ctx->stopCheckpoints = Ctx::getJsonFieldU64(configFileName, debugJson, "stop-checkpoints");
                    ctx->info(0, "will shutdown after " + std::to_string(ctx->stopCheckpoints) + " checkpoints");
                }

                if (debugJson.HasMember("stop-transactions")) {
                    ctx->stopTransactions = Ctx::getJsonFieldU64(configFileName, debugJson, "stop-transactions");
                    ctx->info(0, "will shutdown after " + std::to_string(ctx->stopTransactions) + " transactions");
                }

                if (!ctx->isFlagSet(Ctx::REDO_FLAGS::SCHEMALESS) && (debugJson.HasMember("owner") || debugJson.HasMember("table"))) {
                    debugOwner = Ctx::getJsonFieldS(configFileName, SysUser::NAME_LENGTH, debugJson, "owner");
                    debugTable = Ctx::getJsonFieldS(configFileName, SysObj::NAME_LENGTH, debugJson, "table");
                    ctx->info(0, "will shutdown after committed DML in " + std::string(debugOwner) + "." + debugTable);
                }
            }

            typeConId conId = -1;
            if (readerJson.HasMember("con-id"))
                conId = Ctx::getJsonFieldI16(configFileName, readerJson, "con-id");

            if (sourceJson.HasMember("transaction-max-mb")) {
                uint64_t transactionMaxMb = Ctx::getJsonFieldU64(configFileName, sourceJson, "transaction-max-mb");
                if (transactionMaxMb > memoryMaxMb)
                    throw ConfigurationException(30001, "bad JSON, invalid \"transaction-max-mb\" value: " +
                                                        std::to_string(transactionMaxMb) + ", expected: smaller than \"max-mb\" (" +
                                                        std::to_string(memoryMaxMb) + ")");
                ctx->transactionSizeMax = transactionMaxMb * 1024 * 1024;
            }

            // MEMORY MANAGER
            ctx->initialize(memoryMinMb, memoryMaxMb, memoryReadBufferMaxMb, memoryReadBufferMinMb, memorySwapMb, memoryUnswapBufferMinMb,
                            memoryWriteBufferMaxMb, memoryWriteBufferMinMb);

            // METADATA
            Metadata* metadata = new Metadata(ctx, locales, name, conId, startScn,
                                              startSequence, startTime, startTimeRel);
            metadatas.push_back(metadata);
            metadata->resetElements();
            if (debugOwner != nullptr)
                metadata->users.insert(std::string(debugOwner));

            if (debugOwner != nullptr && debugTable != nullptr)
                metadata->addElement(debugOwner, debugTable, DbTable::OPTIONS::DEBUG_TABLE);
            if (ctx->isFlagSet(Ctx::REDO_FLAGS::ADAPTIVE_SCHEMA))
                metadata->addElement(".*", ".*", 0);

            if (stateType == State::TYPE_DISK) {
                metadata->state = new StateDisk(ctx, statePath);
                metadata->stateDisk = new StateDisk(ctx, "scripts");
                metadata->serializer = new SerializerJson();
            }

            // CHECKPOINT
            auto checkpoint = new Checkpoint(ctx, metadata, std::string(alias) + "-checkpoint", configFileName,
                                             configFileStat.st_mtime);
            checkpoints.push_back(checkpoint);
            ctx->spawnThread(checkpoint);

            // MEMORY MANAGER
            auto memoryManager = new MemoryManager(ctx, std::string(alias) + "-memory-manager", memorySwapPath);
            memoryManager->initialize();
            memoryManagers.push_back(memoryManager);
            ctx->spawnThread(memoryManager);

            // TRANSACTION BUFFER
            TransactionBuffer* transactionBuffer = new TransactionBuffer(ctx);
            transactionBuffers.push_back(transactionBuffer);

            // METRICS
            if (sourceJson.HasMember("metrics")) {
                const rapidjson::Value& metricsJson = Ctx::getJsonFieldO(configFileName, sourceJson, "metrics");

                if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                    static const char* metricsNames[]{"type", "bind", "tag-names", nullptr};
                    Ctx::checkJsonFields(configFileName, metricsJson, metricsNames);
                }

                if (metricsJson.HasMember("type")) {
                    const char* metricsType = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, metricsJson, "type");
                    uint tagNames = Metrics::TAG_NAMES::NONE;

                    if (metricsJson.HasMember("tag-names")) {
                        const char* tagNamesStr = Ctx::getJsonFieldS(configFileName, Ctx::JSON_TOPIC_LENGTH, metricsJson, "tag-names");

                        if (strcmp(tagNamesStr, "none") == 0)
                            tagNames = Metrics::TAG_NAMES::NONE;
                        else if (strcmp(tagNamesStr, "filter") == 0)
                            tagNames = Metrics::TAG_NAMES::FILTER;
                        else if (strcmp(tagNamesStr, "sys") == 0)
                            tagNames = Metrics::TAG_NAMES::SYS;
                        else if (strcmp(tagNamesStr, "all") == 0)
                            tagNames = Metrics::TAG_NAMES::FILTER | Metrics::TAG_NAMES::SYS;
                        else
                            throw ConfigurationException(30001, "bad JSON, invalid \"tag-names\" value: " + std::string(tagNamesStr) +
                                                                ", expected: one of {\"all\", \"filter\", \"none\", \"sys\"}");
                    }

                    if (strcmp(metricsType, "prometheus") == 0) {
#ifdef LINK_LIBRARY_PROMETHEUS
                        const char* prometheusBind = Ctx::getJsonFieldS(configFileName, Ctx::JSON_TOPIC_LENGTH, metricsJson, "bind");

                        ctx->metrics = new MetricsPrometheus(tagNames, prometheusBind);
                        ctx->metrics->initialize(ctx);
#else
                        throw ConfigurationException(30001, "bad JSON, invalid \"type\" value: \"" + std::string(metricsType) +
                                                    "\", expected: not \"prometheus\" since the code is not compiled");
#endif /*LINK_LIBRARY_PROMETHEUS*/
                    } else {
                        throw ConfigurationException(30001, "bad JSON, invalid \"type\" value: \"" + std::string(metricsType) +
                                                            "\", expected: one of {\"prometheus\"}");
                    }
                }
            }

            // FORMAT
            const rapidjson::Value& formatJson = Ctx::getJsonFieldO(configFileName, sourceJson, "format");

            if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                static const char* formatNames[]{"db", "attributes", "interval-dts", "interval-ytm", "message", "rid", "xid",
                                                 "timestamp", "timestamp-tz", "timestamp-all", "char", "scn", "scn-type",
                                                 "unknown", "schema", "column", "unknown-type", "flush-buffer", "type",
                                                 nullptr};
                Ctx::checkJsonFields(configFileName, formatJson, formatNames);
            }

            uint64_t dbFormat = Builder::DB_FORMAT::DB_DEFAULT;
            if (formatJson.HasMember("db")) {
                dbFormat = Ctx::getJsonFieldU64(configFileName, formatJson, "db");
                if (dbFormat > 3)
                    throw ConfigurationException(30001, "bad JSON, invalid \"db\" value: " + std::to_string(dbFormat) +
                                                        ", expected: one of {0 .. 3}");
            }

            uint64_t attributesFormat = Builder::ATTRIBUTES_FORMAT::ATTR_DEFAULT;
            if (formatJson.HasMember("attributes")) {
                attributesFormat = Ctx::getJsonFieldU64(configFileName, formatJson, "attributes");
                if (attributesFormat > 7)
                    throw ConfigurationException(30001, "bad JSON, invalid \"attributes\" value: " + std::to_string(attributesFormat) +
                                                        ", expected: one of {0 .. 7}");
            }

            Builder::INTERVAL_DTS_FORMAT intervalDtsFormat = Builder::INTERVAL_DTS_FORMAT::DTS_UNIX_NANO;
            if (formatJson.HasMember("interval-dts")) {
                uint val = Ctx::getJsonFieldU(configFileName, formatJson, "interval-dts");
                if (val > 10)
                    throw ConfigurationException(30001, "bad JSON, invalid \"interval-dts\" value: " + std::to_string(val) +
                                                        ", expected: one of {0 .. 10}");
                intervalDtsFormat = static_cast<Builder::INTERVAL_DTS_FORMAT>(val);
            }

            Builder::INTERVAL_YTM_FORMAT intervalYtmFormat = Builder::INTERVAL_YTM_FORMAT::YTM_MONTHS;
            if (formatJson.HasMember("interval-ytm")) {
                uint val = Ctx::getJsonFieldU(configFileName, formatJson, "interval-ytm");
                if (val > 4)
                    throw ConfigurationException(30001, "bad JSON, invalid \"interval-ytm\" value: " + std::to_string(val) +
                                                        ", expected: one of {0 .. 4}");
                intervalYtmFormat = static_cast<Builder::INTERVAL_YTM_FORMAT>(val);
            }

            uint messageFormat = Builder::MESSAGE_FORMAT::MSG_DEFAULT;
            if (formatJson.HasMember("message")) {
                messageFormat = Ctx::getJsonFieldU(configFileName, formatJson, "message");
                if (messageFormat > 31)
                    throw ConfigurationException(30001, "bad JSON, invalid \"message\" value: " + std::to_string(messageFormat) +
                                                        ", expected: one of {0 .. 31}");
                if ((messageFormat & Builder::MESSAGE_FORMAT::MSG_FULL) != 0 &&
                    (messageFormat & (Builder::MESSAGE_FORMAT::MSG_SKIP_BEGIN | Builder::MESSAGE_FORMAT::MSG_SKIP_COMMIT)) != 0)
                    throw ConfigurationException(30001, "bad JSON, invalid \"message\" value: " + std::to_string(messageFormat) +
                                                        ", expected: BEGIN/COMMIT flag is unset (" + std::to_string(Builder::MESSAGE_FORMAT::MSG_SKIP_BEGIN) +
                                                        "/" + std::to_string(Builder::MESSAGE_FORMAT::MSG_SKIP_COMMIT) + ") together with FULL mode (" +
                                                        std::to_string(Builder::MESSAGE_FORMAT::MSG_FULL) + ")");
            }

            Builder::RID_FORMAT ridFormat = Builder::RID_FORMAT::RID_SKIP;
            if (formatJson.HasMember("rid")) {
                uint val = Ctx::getJsonFieldU(configFileName, formatJson, "rid");
                if (val > 1)
                    throw ConfigurationException(30001, "bad JSON, invalid \"rid\" value: " + std::to_string(val) +
                                                        ", expected: one of {0, 1}");
                ridFormat = static_cast<Builder::RID_FORMAT>(val);
            }

            Builder::XID_FORMAT xidFormat = Builder::XID_FORMAT::XID_TEXT_HEX;
            if (formatJson.HasMember("xid")) {
                uint val = Ctx::getJsonFieldU(configFileName, formatJson, "xid");
                if (val > 2)
                    throw ConfigurationException(30001, "bad JSON, invalid \"xid\" value: " + std::to_string(val) +
                                                        ", expected: one of {0 .. 2}");
                xidFormat = static_cast<Builder::XID_FORMAT>(val);
            }

            Builder::TIMESTAMP_FORMAT timestampFormat = Builder::TIMESTAMP_FORMAT::TMSTP_UNIX_NANO;
            if (formatJson.HasMember("timestamp")) {
                uint val  = Ctx::getJsonFieldU(configFileName, formatJson, "timestamp");
                if (val > 15)
                    throw ConfigurationException(30001, "bad JSON, invalid \"timestamp\" value: " + std::to_string(val) +
                                                        ", expected: one of {0 .. 15}");
                timestampFormat = static_cast<Builder::TIMESTAMP_FORMAT>(val);
            }

            Builder::TIMESTAMP_TZ_FORMAT timestampTzFormat = Builder::TIMESTAMP_TZ_FORMAT::TMSTP_TZ_UNIX_NANO_STRING;
            if (formatJson.HasMember("timestamp-tz")) {
                uint val = Ctx::getJsonFieldU(configFileName, formatJson, "timestamp-tz");
                if (val > 11)
                    throw ConfigurationException(30001, "bad JSON, invalid \"timestamp-tz\" value: " + std::to_string(val) +
                                                        ", expected: one of {0 .. 11}");
                timestampTzFormat = static_cast<Builder::TIMESTAMP_TZ_FORMAT>(val);
            }

            Builder::TIMESTAMP_ALL timestampAll = Builder::TIMESTAMP_ALL::TIMESTAMP_JUST_BEGIN;
            if (formatJson.HasMember("timestamp-all")) {
                uint val = Ctx::getJsonFieldU64(configFileName, formatJson, "timestamp-all");
                if (val > 1)
                    throw ConfigurationException(30001, "bad JSON, invalid \"timestamp-all\" value: " + std::to_string(val) +
                                                        ", expected: one of {0, 1}");
                timestampAll = static_cast<Builder::TIMESTAMP_ALL>(val);
            }

            Builder::CHAR_FORMAT charFormat = Builder::CHAR_FORMAT::UTF8;
            if (formatJson.HasMember("char")) {
                uint val = Ctx::getJsonFieldU(configFileName, formatJson, "char");
                if (val > 3)
                    throw ConfigurationException(30001, "bad JSON, invalid \"char\" value: " + std::to_string(val) +
                                                        ", expected: one of {0 .. 3}");
                charFormat = static_cast<Builder::CHAR_FORMAT>(val);
            }

            Builder::SCN_FORMAT scnFormat = Builder::SCN_FORMAT::SCN_NUMERIC;
            if (formatJson.HasMember("scn")) {
                uint val = Ctx::getJsonFieldU(configFileName, formatJson, "scn");
                if (val > 1)
                    throw ConfigurationException(30001, "bad JSON, invalid \"scn\" value: " + std::to_string(scnFormat) +
                                                        ", expected: one of {0, 1}");
                scnFormat = static_cast<Builder::SCN_FORMAT>(val);
            }

            uint scnType = Builder::SCN_TYPE::SCN_NONE;
            if (formatJson.HasMember("scn-type")) {
                scnType = Ctx::getJsonFieldU64(configFileName, formatJson, "scn-type");
                if (scnType > 3)
                    throw ConfigurationException(30001, "bad JSON, invalid \"scn-type\" value: " + std::to_string(scnType) +
                                                        ", expected: one of {0, 3}");
            }

            Builder::UNKNOWN_FORMAT unknownFormat = Builder::UNKNOWN_FORMAT::UNKNOWN_QUESTION_MARK;
            if (formatJson.HasMember("unknown")) {
                uint val = Ctx::getJsonFieldU(configFileName, formatJson, "unknown");
                if (val > 1)
                    throw ConfigurationException(30001, "bad JSON, invalid \"unknown\" value: " + std::to_string(val) +
                                                        ", expected: one of {0, 1}");
                unknownFormat = static_cast<Builder::UNKNOWN_FORMAT>(val);
            }

            uint schemaFormat = Builder::SCHEMA_FORMAT::SCHEMA_DEFAULT;
            if (formatJson.HasMember("schema")) {
                schemaFormat = Ctx::getJsonFieldU(configFileName, formatJson, "schema");
                if (schemaFormat > 7)
                    throw ConfigurationException(30001, "bad JSON, invalid \"schema\" value: " + std::to_string(schemaFormat) +
                                                        ", expected: one of {0 .. 7}");
            }

            Builder::COLUMN_FORMAT columnFormat = Builder::COLUMN_FORMAT::CHANGED;
            if (formatJson.HasMember("column")) {
                uint val = Ctx::getJsonFieldU(configFileName, formatJson, "column");
                if (val > 2)
                    throw ConfigurationException(30001, "bad JSON, invalid \"column\" value: " + std::to_string(val) +
                                                        ", expected: one of {0 .. 2}");

                if (ctx->isFlagSet(Ctx::REDO_FLAGS::SCHEMALESS) && val != 0)
                    throw ConfigurationException(30001, "bad JSON, invalid \"column\" value: " + std::to_string(val) +
                                                        ", expected: not used when flags has set schemaless mode (flags: " + std::to_string(ctx->flags) + ")");
                columnFormat = static_cast<Builder::COLUMN_FORMAT>(val);
            }

            Builder::UNKNOWN_TYPE unknownType = Builder::UNKNOWN_TYPE::UNKNOWN_HIDE;
            if (formatJson.HasMember("unknown-type")) {
                uint val = Ctx::getJsonFieldU(configFileName, formatJson, "unknown-type");
                if (val > 1)
                    throw ConfigurationException(30001, "bad JSON, invalid \"unknown-type\" value: " + std::to_string(val) +
                                                        ", expected: one of {0, 1}");
                unknownType = static_cast<Builder::UNKNOWN_TYPE>(val);
            }

            uint64_t flushBuffer = 1048576;
            if (formatJson.HasMember("flush-buffer"))
                flushBuffer = Ctx::getJsonFieldU64(configFileName, formatJson, "flush-buffer");

            const char* formatType = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, formatJson, "type");

            Builder* builder;
            if (strcmp("json", formatType) == 0) {
                builder = new BuilderJson(ctx, locales, metadata, dbFormat, attributesFormat,
                                          intervalDtsFormat, intervalYtmFormat, messageFormat,
                                          ridFormat, xidFormat, timestampFormat,
                                          timestampTzFormat, timestampAll, charFormat, scnFormat,
                                          scnType, unknownFormat, schemaFormat, columnFormat,
                                          unknownType, flushBuffer);
            } else if (strcmp("protobuf", formatType) == 0) {
#ifdef LINK_LIBRARY_PROTOBUF
                builder = new BuilderProtobuf(ctx, locales, metadata, dbFormat, attributesFormat,
                                              intervalDtsFormat, intervalYtmFormat, messageFormat,
                                              ridFormat, xidFormat, timestampFormat,
                                              timestampTzFormat, timestampAll, charFormat, scnFormat,
                                              scnType, unknownFormat, schemaFormat,
                                              columnFormat, unknownType, flushBuffer);
#else
                throw ConfigurationException(30001, "bad JSON, invalid \"format\" value: " + std::string(formatType) +
                                             ", expected: not \"protobuf\" since the code is not compiled");
#endif /* LINK_LIBRARY_PROTOBUF */
            } else
                throw ConfigurationException(30001, "bad JSON, invalid \"format\" value: " + std::string(formatType) +
                                                    ", expected: \"protobuf\" or \"json\"");
            builders.push_back(builder);

            // READER
            const char* readerType = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, readerJson, "type");
            void (* archGetLog)(Replicator* replicator) = Replicator::archGetLogPath;

            if (sourceJson.HasMember("redo-read-sleep-us"))
                ctx->redoReadSleepUs = Ctx::getJsonFieldU64(configFileName, sourceJson, "redo-read-sleep-us");

            if (sourceJson.HasMember("arch-read-sleep-us"))
                ctx->archReadSleepUs = Ctx::getJsonFieldU64(configFileName, sourceJson, "arch-read-sleep-us");

            if (sourceJson.HasMember("arch-read-tries")) {
                ctx->archReadTries = Ctx::getJsonFieldU64(configFileName, sourceJson, "arch-read-tries");
                if (ctx->archReadTries < 1 || ctx->archReadTries > 1000000000)
                    throw ConfigurationException(30001, "bad JSON, invalid \"arch-read-tries\" value: " +
                                                        std::to_string(ctx->archReadTries) + ", expected: one of: {1 .. 1000000000}");
            }

            if (sourceJson.HasMember("redo-verify-delay-us"))
                ctx->redoVerifyDelayUs = Ctx::getJsonFieldU64(configFileName, sourceJson, "redo-verify-delay-us");

            if (sourceJson.HasMember("refresh-interval-us"))
                ctx->refreshIntervalUs = Ctx::getJsonFieldU64(configFileName, sourceJson, "refresh-interval-us");

            if (readerJson.HasMember("redo-copy-path"))
                ctx->redoCopyPath = Ctx::getJsonFieldS(configFileName, Ctx::MAX_PATH_LENGTH, readerJson, "redo-copy-path");

            if (readerJson.HasMember("db-timezone")) {
                const char* dbTimezone = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, readerJson, "db-timezone");
                if (!ctx->parseTimezone(dbTimezone, ctx->dbTimezone))
                    throw ConfigurationException(30001, "bad JSON, invalid \"db-timezone\" value: " + std::string(dbTimezone) +
                                                        ", expected value: {\"+/-HH:MM\"}");
            }

            if (readerJson.HasMember("host-timezone")) {
                const char* hostTimezone = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, readerJson, "host-timezone");
                if (!ctx->parseTimezone(hostTimezone, ctx->hostTimezone))
                    throw ConfigurationException(30001, "bad JSON, invalid \"host-timezone\" value: " + std::string(hostTimezone) +
                                                        ", expected value: {\"+/-HH:MM\"}");
            }

            if (readerJson.HasMember("log-timezone")) {
                const char* logTimezone = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, readerJson, "log-timezone");
                if (!ctx->parseTimezone(logTimezone, ctx->logTimezone))
                    throw ConfigurationException(30001, "bad JSON, invalid \"log-timezone\" value: " + std::string(logTimezone) +
                                                        ", expected value: {\"+/-HH:MM\"}");
            }


            if (strcmp(readerType, "online") == 0) {
#ifdef LINK_LIBRARY_OCI
                const char* user = Ctx::getJsonFieldS(configFileName, Ctx::JSON_USERNAME_LENGTH, readerJson, "user");
                const char* password = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PASSWORD_LENGTH, readerJson, "password");
                const char* server = Ctx::getJsonFieldS(configFileName, Ctx::JSON_SERVER_LENGTH, readerJson, "server");
                bool keepConnection = false;

                if (sourceJson.HasMember("arch")) {
                    const char* arch = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, sourceJson, "arch");

                    if (strcmp(arch, "path") == 0)
                        archGetLog = Replicator::archGetLogPath;
                    else if (strcmp(arch, "online") == 0) {
                        archGetLog = ReplicatorOnline::archGetLogOnline;
                    } else if (strcmp(arch, "online-keep") == 0) {
                        archGetLog = ReplicatorOnline::archGetLogOnline;
                        keepConnection = true;
                    } else
                        throw ConfigurationException(30001, "bad JSON, invalid \"arch\" value: " + std::string(arch) +
                                                     ", expected: one of {\"path\", \"online\", \"online-keep\"}");
                } else
                    archGetLog = ReplicatorOnline::archGetLogOnline;

                replicator = new ReplicatorOnline(ctx, archGetLog, builder, metadata,
                                                  transactionBuffer, alias, name, user, password,
                                                  server, keepConnection);
                builder->initialize();
                replicator->initialize();
                mainProcessMapping(readerJson);
#else
                throw ConfigurationException(30001, "bad JSON, invalid \"type\" value: " + std::string(readerType) +
                                                    ", expected: not \"online\" since the code is not compiled");
#endif /*LINK_LIBRARY_OCI*/

            } else if (strcmp(readerType, "offline") == 0) {
                if (strcmp(startTime, "") != 0)
                    throw ConfigurationException(30001, "bad JSON, invalid \"start-time\" value: " + std::string(startTime) +
                                                        ", expected: unset when reader \"type\" is \"offline\"");
                if (startTimeRel > 0)
                    throw ConfigurationException(30001, "bad JSON, invalid \"start-time-rel\" value: " + std::to_string(startTimeRel) +
                                                        ", expected: unset when reader \"type\" is \"offline\"");

                replicator = new Replicator(ctx, archGetLog, builder, metadata, transactionBuffer,
                                            alias, name);
                builder->initialize();
                replicator->initialize();
                mainProcessMapping(readerJson);

            } else if (strcmp(readerType, "batch") == 0) {
                if (strcmp(startTime, "") != 0)
                    throw ConfigurationException(30001, "bad JSON, invalid \"start-time\" value: " + std::string(startTime) +
                                                        ", expected: unset when reader \"type\" is \"batch\"");
                if (startTimeRel > 0)
                    throw ConfigurationException(30001, "bad JSON, invalid \"start-time-rel\" value: " + std::to_string(startTimeRel) +
                                                        ", expected: unset when reader \"type\" is \"offline\"");

                archGetLog = Replicator::archGetLogList;
                replicator = new ReplicatorBatch(ctx, archGetLog, builder, metadata,
                                                 transactionBuffer, alias, name);
                builder->initialize();
                replicator->initialize();

                const rapidjson::Value& redoLogBatchArrayJson = Ctx::getJsonFieldA(configFileName, readerJson, "redo-log");

                for (rapidjson::SizeType k = 0; k < redoLogBatchArrayJson.Size(); ++k)
                    replicator->addRedoLogsBatch(Ctx::getJsonFieldS(configFileName, Ctx::MAX_PATH_LENGTH, redoLogBatchArrayJson,
                                                                    "redo-log", k));

            } else
                throw ConfigurationException(30001, "bad JSON, invalid \"type\" value: " + std::string(readerType) +
                                                    ", expected: one of {\"online\", \"offline\", \"batch\"}");

            if (sourceJson.HasMember("filter")) {
                const rapidjson::Value& filterJson = Ctx::getJsonFieldO(configFileName, sourceJson, "filter");

                if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                    static const char* filterNames[]{"table", "skip-xid", "separator", "dump-xid", nullptr};
                    Ctx::checkJsonFields(configFileName, filterJson, filterNames);
                }

                std::string separator{","};
                if (filterJson.HasMember("separator"))
                    separator = Ctx::getJsonFieldS(configFileName, Ctx::JSON_FORMAT_SEPARATOR_LENGTH, filterJson, "separator");

                if (filterJson.HasMember("table") && !ctx->isFlagSet(Ctx::REDO_FLAGS::SCHEMALESS)) {
                    const rapidjson::Value& tableArrayJson = Ctx::getJsonFieldA(configFileName, filterJson, "table");

                    for (rapidjson::SizeType k = 0; k < tableArrayJson.Size(); ++k) {
                        const rapidjson::Value& tableElementJson = Ctx::getJsonFieldO(configFileName, tableArrayJson, "table", k);

                        if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                            static const char* tableElementNames[]{"owner", "table", "key", "condition", "tag", nullptr};
                            Ctx::checkJsonFields(configFileName, tableElementJson, tableElementNames);
                        }

                        const char* owner = Ctx::getJsonFieldS(configFileName, SysUser::NAME_LENGTH, tableElementJson, "owner");
                        const char* table = Ctx::getJsonFieldS(configFileName, SysObj::NAME_LENGTH, tableElementJson, "table");
                        SchemaElement* element = metadata->addElement(owner, table, 0);

                        metadata->users.insert(owner);

                        if (tableElementJson.HasMember("key")) {
                            element->key = Ctx::getJsonFieldS(configFileName, Ctx::JSON_KEY_LENGTH, tableElementJson, "key");
                            element->parseKey(element->key, separator);
                        };

                        if (tableElementJson.HasMember("condition"))
                            element->condition = Ctx::getJsonFieldS(configFileName, Ctx::JSON_CONDITION_LENGTH, tableElementJson,
                                                                    "condition");

                        if (tableElementJson.HasMember("tag")) {
                            element->tag = Ctx::getJsonFieldS(configFileName, Ctx::JSON_TAG_LENGTH, tableElementJson, "tag");
                            element->parseTag(element->tag, separator);
                        }
                    }
                }

                if (filterJson.HasMember("skip-xid")) {
                    const rapidjson::Value& skipXidArrayJson = Ctx::getJsonFieldA(configFileName, filterJson, "skip-xid");
                    for (rapidjson::SizeType k = 0; k < skipXidArrayJson.Size(); ++k) {
                        typeXid xid(Ctx::getJsonFieldS(configFileName, Ctx::JSON_XID_LENGTH, skipXidArrayJson, "skip-xid", k));
                        ctx->info(0, "adding XID to skip list: " + xid.toString());
                        transactionBuffer->skipXidList.insert(xid);
                    }
                }

                if (filterJson.HasMember("dump-xid")) {
                    const rapidjson::Value& dumpXidArrayJson = Ctx::getJsonFieldA(configFileName, filterJson, "dump-xid");
                    for (rapidjson::SizeType k = 0; k < dumpXidArrayJson.Size(); ++k) {
                        typeXid xid(Ctx::getJsonFieldS(configFileName, Ctx::JSON_XID_LENGTH, dumpXidArrayJson, "dump-xid", k));
                        ctx->info(0, "adding XID to dump list: " + xid.toString());
                        transactionBuffer->dumpXidList.insert(xid);
                    }
                }
            }

            if (readerJson.HasMember("log-archive-format")) {
                replicator->metadata->logArchiveFormatCustom = true;
                replicator->metadata->logArchiveFormat = Ctx::getJsonFieldS(configFileName, DbTable::VPARAMETER_LENGTH, readerJson,
                                                                            "log-archive-format");
            }

            metadata->commitElements();
            replicators.push_back(replicator);
            ctx->spawnThread(replicator);
            replicator = nullptr;
        }

        // Iterate through targets
        const rapidjson::Value& targetArrayJson = Ctx::getJsonFieldA(configFileName, document, "target");
        if (targetArrayJson.Size() != 1) {
            throw ConfigurationException(30001, "bad JSON, invalid \"target\" value: " + std::to_string(targetArrayJson.Size()) +
                                                " elements, expected: 1 element");
        }

        for (rapidjson::SizeType j = 0; j < targetArrayJson.Size(); ++j) {
            const rapidjson::Value& targetJson = targetArrayJson[j];
            const char* alias = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, targetJson, "alias");
            const char* source = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, targetJson, "source");

            ctx->info(0, "adding target: " + std::string(alias));
            Replicator* replicator2 = nullptr;
            for (Replicator* replicatorTmp: replicators)
                if (replicatorTmp->alias == source)
                    replicator2 = replicatorTmp;
            if (replicator2 == nullptr)
                throw ConfigurationException(30001, "bad JSON, invalid \"source\" value: " + std::string(source) +
                                                    ", expected: value used earlier in \"source\" field");

            // Writer
            Writer* writer;
            const rapidjson::Value& writerJson = Ctx::getJsonFieldO(configFileName, targetJson, "writer");
            const char* writerType = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, writerJson, "type");

            if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                static const char* writerNames[]{"type", "poll-interval-us", "queue-size", "max-file-size", "timestamp-format",
                                                 "output", "new-line", "append", "max-message-mb", "topic", "properties",
                                                 "uri", nullptr};
                Ctx::checkJsonFields(configFileName, writerJson, writerNames);
            }

            if (writerJson.HasMember("poll-interval-us")) {
                ctx->pollIntervalUs = Ctx::getJsonFieldU64(configFileName, writerJson, "poll-interval-us");
                if (ctx->pollIntervalUs < 100 || ctx->pollIntervalUs > 3600000000)
                    throw ConfigurationException(30001, "bad JSON, invalid \"poll-interval-us\" value: " +
                                                        std::to_string(ctx->pollIntervalUs) + ", expected: one of {100 .. 3600000000}");
            }

            if (writerJson.HasMember("queue-size")) {
                ctx->queueSize = Ctx::getJsonFieldU64(configFileName, writerJson, "queue-size");
                if (ctx->queueSize < 1 || ctx->queueSize > 1000000)
                    throw ConfigurationException(30001, "bad JSON, invalid \"queue-size\" value: " + std::to_string(ctx->queueSize) +
                                                        ", expected: one of {1 .. 1000000}");
            }

            if (strcmp(writerType, "file") == 0) {
                uint64_t maxFileSize = 0;
                if (writerJson.HasMember("max-file-size"))
                    maxFileSize = Ctx::getJsonFieldU64(configFileName, writerJson, "max-file-size");

                const char* timestampFormat = "%F_%T";
                if (writerJson.HasMember("timestamp-format"))
                    timestampFormat = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, writerJson, "timestamp-format");

                const char* output = "";
                if (writerJson.HasMember("output"))
                    output = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, writerJson, "output");
                else if (maxFileSize > 0)
                    throw ConfigurationException(30001, "bad JSON, invalid \"output\" value: " + std::string(output) +
                                                        ", expected: to be set when \"max-file-size\" is set (" + std::to_string(maxFileSize) + ")");

                uint64_t newLine = 1;
                if (writerJson.HasMember("new-line")) {
                    newLine = Ctx::getJsonFieldU64(configFileName, writerJson, "new-line");
                    if (newLine > 2)
                        throw ConfigurationException(30001, "bad JSON, invalid \"new-line\" value: " + std::to_string(newLine) +
                                                            ", expected: one of {0 .. 2}");
                }

                uint64_t append = 1;
                if (writerJson.HasMember("append")) {
                    append = Ctx::getJsonFieldU64(configFileName, writerJson, "append");
                    if (append > 1)
                        throw ConfigurationException(30001, "bad JSON, invalid \"append\" value: " + std::to_string(append) +
                                                            ", expected: one of {0, 1}");
                }

                writer = new WriterFile(ctx, std::string(alias) + "-writer", replicator2->database,
                                        replicator2->builder, replicator2->metadata, output, timestampFormat,
                                        maxFileSize, newLine, append);
            } else if (strcmp(writerType, "discard") == 0) {
                writer = new WriterDiscard(ctx, std::string(alias) + "-writer", replicator2->database,
                                           replicator2->builder, replicator2->metadata);
            } else if (strcmp(writerType, "kafka") == 0) {
#ifdef LINK_LIBRARY_RDKAFKA
                uint64_t maxMessageMb = 100;
                if (writerJson.HasMember("max-message-mb")) {
                    maxMessageMb = Ctx::getJsonFieldU64(configFileName, writerJson, "max-message-mb");
                    if (maxMessageMb < 1 || maxMessageMb > WriterKafka::MAX_KAFKA_MESSAGE_MB)
                        throw ConfigurationException(30001, "bad JSON, invalid \"max-message-mb\" value: " + std::to_string(maxMessageMb) +
                                                            ", expected: one of {1 .. " + std::to_string(WriterKafka::MAX_KAFKA_MESSAGE_MB) + "}");
                }
                replicator2->builder->setMaxMessageMb(maxMessageMb);

                const char* topic = Ctx::getJsonFieldS(configFileName, Ctx::JSON_TOPIC_LENGTH, writerJson, "topic");

                writer = new WriterKafka(ctx, std::string(alias) + "-writer", replicator2->database,
                                         replicator2->builder, replicator2->metadata, topic);

                if (writerJson.HasMember("properties")) {
                    const rapidjson::Value& propertiesJson = Ctx::getJsonFieldO(configFileName, writerJson, "properties");

                    for (rapidjson::Value::ConstMemberIterator itr = propertiesJson.MemberBegin(); itr != propertiesJson.MemberEnd(); ++itr) {
                        const char* key = itr->name.GetString();
                        const char* value = itr->value.GetString();
                        reinterpret_cast<WriterKafka*>(writer)->addProperty(key, value);
                    }
                }
#else
                throw ConfigurationException(30001, "bad JSON, invalid \"type\" value: " + std::string(writerType) +
                                             ", expected: not \"kafka\" since the code is not compiled");
#endif /* LINK_LIBRARY_RDKAFKA */
            } else if (strcmp(writerType, "zeromq") == 0) {
#if defined(LINK_LIBRARY_PROTOBUF) && defined(LINK_LIBRARY_ZEROMQ)
                const char* uri = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, writerJson, "uri");
                StreamZeroMQ* stream = new StreamZeroMQ(ctx, uri);
                stream->initialize();
                writer = new WriterStream(ctx, std::string(alias) + "-writer", replicator2->database,
                                          replicator2->builder, replicator2->metadata, stream);
#else
                throw ConfigurationException(30001, "bad JSON, invalid \"type\" value: " + std::string(writerType) +
                                             ", expected: not \"zeromq\" since the code is not compiled");
#endif /* defined(LINK_LIBRARY_PROTOBUF) && defined(LINK_LIBRARY_ZEROMQ) */
            } else if (strcmp(writerType, "network") == 0) {
#ifdef LINK_LIBRARY_PROTOBUF
                const char* uri = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, writerJson, "uri");

                StreamNetwork* stream = new StreamNetwork(ctx, uri);
                stream->initialize();
                writer = new WriterStream(ctx, std::string(alias) + "-writer", replicator2->database,
                                          replicator2->builder, replicator2->metadata, stream);
#else
                throw ConfigurationException(30001, "bad JSON, invalid \"type\" value: " + std::string(writerType) +
                                             ", expected: not \"network\" since the code is not compiled");
#endif /* LINK_LIBRARY_PROTOBUF */
            } else
                throw ConfigurationException(30001, "bad JSON, invalid \"type\" value: " + std::string(writerType) +
                                                    ", expected: one of {\"file\", \"kafka\", \"zeromq\", \"network\", \"discard\"}");

            writers.push_back(writer);
            writer->initialize();
            ctx->spawnThread(writer);
        }

        ctx->mainLoop();

        if (unlikely(ctx->trace & Ctx::TRACE::THREADS)) {
            std::ostringstream ss;
            ss << std::this_thread::get_id();
            ctx->logTrace(Ctx::TRACE::THREADS, "main (" + ss.str() + ") stop");
        }

        return 0;
    }

    void OpenLogReplicator::mainProcessMapping(const rapidjson::Value& readerJson) {
        if (readerJson.HasMember("path-mapping")) {
            const rapidjson::Value& pathMappingArrayJson = Ctx::getJsonFieldA(configFileName, readerJson, "path-mapping");

            if ((pathMappingArrayJson.Size() % 2) != 0)
                throw ConfigurationException(30001, "bad JSON, invalid \"path-mapping\" value: " +
                                                    std::to_string(pathMappingArrayJson.Size()) + " elements, expected: even number of elements");

            for (rapidjson::SizeType k = 0; k < pathMappingArrayJson.Size() / 2; ++k) {
                const char* sourceMapping = Ctx::getJsonFieldS(configFileName, Ctx::MAX_PATH_LENGTH, pathMappingArrayJson, "path-mapping",
                                                               k * 2);
                const char* targetMapping = Ctx::getJsonFieldS(configFileName, Ctx::MAX_PATH_LENGTH, pathMappingArrayJson, "path-mapping",
                                                               k * 2 + 1);
                replicator->addPathMapping(sourceMapping, targetMapping);
            }
        }
    }
}
