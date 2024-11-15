/* Base class for process to write checkpoint files
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
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "../common/Ctx.h"
#include "../common/exception/ConfigurationException.h"
#include "../common/exception/RuntimeException.h"
#include "../common/DbTable.h"
#include "../common/table/SysObj.h"
#include "../common/table/SysUser.h"
#include "Checkpoint.h"
#include "Metadata.h"
#include "Schema.h"
#include "SchemaElement.h"

namespace OpenLogReplicator {
    Checkpoint::Checkpoint(Ctx* newCtx, Metadata* newMetadata, const std::string& newAlias, const std::string& newConfigFileName, time_t newConfigFileChange) :
            Thread(newCtx, newAlias),
            metadata(newMetadata),
            configFileBuffer(nullptr),
            configFileName(newConfigFileName),
            configFileChange(newConfigFileChange) {
    }

    Checkpoint::~Checkpoint() {
        if (configFileBuffer != nullptr)
            delete[] configFileBuffer;
        configFileBuffer = nullptr;
    }

    void Checkpoint::wakeUp() {
        {
            contextSet(Thread::CONTEXT_MUTEX, Thread::CHECKPOINT_WAKEUP);
            std::unique_lock<std::mutex> lck(mtx);
            condLoop.notify_all();
        }
        contextSet(Thread::CONTEXT_CPU);
    }

    void Checkpoint::trackConfigFile() {
        struct stat configFileStat;
        if (unlikely(stat(configFileName.c_str(), &configFileStat) != 0))
            throw RuntimeException(10003, "file: " + configFileName + " - get metadata returned: " + strerror(errno));

        if (configFileStat.st_mtime == configFileChange)
            return;

        ctx->info(0, "config file changed, reloading");

        try {
            int fid = open(configFileName.c_str(), O_RDONLY);
            if (unlikely(fid == -1))
                throw ConfigurationException(10001, "file: " + configFileName + " - open for read returned: " + strerror(errno));

            if (unlikely(configFileStat.st_size > CONFIG_FILE_MAX_SIZE || configFileStat.st_size == 0))
                throw ConfigurationException(10004, "file: " + configFileName + " - wrong size: " +
                                                    std::to_string(configFileStat.st_size));

            if (configFileBuffer != nullptr)
                delete[] configFileBuffer;

            configFileBuffer = new char[configFileStat.st_size + 1];
            uint64_t bytesRead = read(fid, configFileBuffer, configFileStat.st_size);
            if (unlikely(bytesRead != static_cast<uint64_t>(configFileStat.st_size)))
                throw ConfigurationException(10005, "file: " + configFileName + " - " + std::to_string(bytesRead) +
                                                    " bytes read instead of " + std::to_string(configFileStat.st_size));
            configFileBuffer[configFileStat.st_size] = 0;

            updateConfigFile();

            delete[] configFileBuffer;
            configFileBuffer = nullptr;

        } catch (ConfigurationException& ex) {
            ctx->error(ex.code, ex.msg);
        } catch (DataException& ex) {
            ctx->error(ex.code, ex.msg);
        }

        configFileChange = configFileStat.st_mtime;
    }

    void Checkpoint::updateConfigFile() {
        rapidjson::Document document;
        if (unlikely(document.Parse(configFileBuffer).HasParseError()))
            throw ConfigurationException(20001, "file: " + configFileName + " offset: " + std::to_string(document.GetErrorOffset()) +
                                                " - parse error: " + GetParseError_En(document.GetParseError()));

        if (!metadata->ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
            static const char* documentNames[]{"version", "dump-path", "dump-raw-data", "dump-redo-log", "log-level", "trace",
                                               "source", "target", nullptr};
            Ctx::checkJsonFields(configFileName, document, documentNames);
        }

        const char* version = Ctx::getJsonFieldS(configFileName, Ctx::JSON_PARAMETER_LENGTH, document, "version");
        if (unlikely(strcmp(version, OpenLogReplicator_SCHEMA_VERSION) != 0))
            throw ConfigurationException(30001, "bad JSON, invalid 'version' value: " + std::string(version) + ", expected: " +
                                                OpenLogReplicator_SCHEMA_VERSION);

        // Iterate through sources
        const rapidjson::Value& sourceArrayJson = Ctx::getJsonFieldA(configFileName, document, "source");
        if (unlikely(sourceArrayJson.Size() != 1)) {
            throw ConfigurationException(30001, "bad JSON, invalid 'source' value: " + std::to_string(sourceArrayJson.Size()) +
                                                " elements, expected: 1 element");
        }

        for (rapidjson::SizeType j = 0; j < sourceArrayJson.Size(); ++j) {
            const rapidjson::Value& sourceJson = Ctx::getJsonFieldO(configFileName, sourceArrayJson, "source", j);

            if (!metadata->ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                static const char* sourceNames[]{"alias", "memory", "name", "reader", "flags", "state", "debug",
                                                 "transaction-max-mb", "metrics", "format", "redo-read-sleep-us", "arch-read-sleep-us",
                                                 "arch-read-tries", "redo-verify-delay-us", "refresh-interval-us", "arch",
                                                 "filter", nullptr};
                Ctx::checkJsonFields(configFileName, sourceJson, sourceNames);
            }

            metadata->resetElements();

            const char* debugOwner = nullptr;
            const char* debugTable;

            if (sourceJson.HasMember("debug")) {
                const rapidjson::Value& debugJson = Ctx::getJsonFieldO(configFileName, sourceJson, "debug");

                if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                    static const char* debugNames[]{"stop-log-switches", "stop-checkpoints", "stop-transactions", "owner", "table",
                                                    nullptr};
                    Ctx::checkJsonFields(configFileName, debugJson, debugNames);
                }

                if (!ctx->isFlagSet(Ctx::REDO_FLAGS::SCHEMALESS) && (debugJson.HasMember("owner") || debugJson.HasMember("table"))) {
                    debugOwner = Ctx::getJsonFieldS(configFileName, SysUser::NAME_LENGTH, debugJson, "owner");
                    debugTable = Ctx::getJsonFieldS(configFileName, SysObj::NAME_LENGTH, debugJson, "table");
                    ctx->info(0, "will shutdown after committed DML in " + std::string(debugOwner) + "." + debugTable);
                }
            }

            std::set<std::string> users;
            if (debugOwner != nullptr && debugTable != nullptr) {
                metadata->addElement(debugOwner, debugTable, DbTable::OPTIONS::DEBUG_TABLE);
                users.insert(std::string(debugOwner));
            }
            if (ctx->isFlagSet(Ctx::REDO_FLAGS::ADAPTIVE_SCHEMA))
                metadata->addElement(".*", ".*", 0);

            if (sourceJson.HasMember("filter")) {
                const rapidjson::Value& filterJson = Ctx::getJsonFieldO(configFileName, sourceJson, "filter");

                if (!ctx->isDisableChecksSet(Ctx::DISABLE_CHECKS::JSON_TAGS)) {
                    static const char* filterNames[]{"table", "skip-xid", "separator", "dump-xid", nullptr};
                    Ctx::checkJsonFields(configFileName, filterJson, filterNames);
                }

                if (filterJson.HasMember("table") && !ctx->isFlagSet(Ctx::REDO_FLAGS::SCHEMALESS)) {
                    const rapidjson::Value& tableArrayJson = Ctx::getJsonFieldA(configFileName, filterJson, "table");

                    std::string separator{","};
                    if (filterJson.HasMember("separator"))
                        separator = Ctx::getJsonFieldS(configFileName, Ctx::JSON_FORMAT_SEPARATOR_LENGTH, filterJson, "separator");

                    for (rapidjson::SizeType k = 0; k < tableArrayJson.Size(); ++k) {
                        const rapidjson::Value& tableElementJson = Ctx::getJsonFieldO(configFileName, tableArrayJson, "table", k);

                        const char* owner = Ctx::getJsonFieldS(configFileName, SysUser::NAME_LENGTH, tableElementJson, "owner");
                        const char* table = Ctx::getJsonFieldS(configFileName, SysObj::NAME_LENGTH, tableElementJson, "table");
                        SchemaElement* element = metadata->addElement(owner, table, 0);

                        users.insert(owner);

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

                    for (auto& user: metadata->users)
                        if (unlikely(users.find(user) == users.end()))
                            throw ConfigurationException(20007, "file: " + configFileName + " - " + user + " is missing");

                    for (auto& user: users)
                        if (unlikely(metadata->users.find(user) == metadata->users.end()))
                            throw ConfigurationException(20007, "file: " + configFileName + " - " + user + " is redundant");

                    users.clear();
                }
            }
        }

        ctx->info(0, "scanning objects which match the configuration file");
        // Suspend transaction processing for the schema update
        {
            contextSet(Thread::CONTEXT_TRAN, Thread::REASON_TRAN);
            std::unique_lock<std::mutex> lckTransaction(metadata->mtxTransaction);
            metadata->commitElements();
            metadata->schema->purgeMetadata();

            // Mark all tables as touched to force a schema update
            for (const auto& it: metadata->schema->sysObjMapRowId)
                metadata->schema->touchTable(it.second->obj);

            std::vector<std::string> msgs;
            metadata->buildMaps(msgs);
            for (const auto& msg: msgs)
                ctx->info(0, "- found: " + msg);

            metadata->schema->resetTouched();
        }
        contextSet(Thread::CONTEXT_CPU);
    }

    void Checkpoint::run() {
        if (unlikely(ctx->trace & Ctx::TRACE::THREADS)) {
            std::ostringstream ss;
            ss << std::this_thread::get_id();
            ctx->logTrace(Ctx::TRACE::THREADS, "checkpoint (" + ss.str() + ") start");
        }

        try {
            while (!ctx->hardShutdown) {
                metadata->writeCheckpoint(this, false);
                metadata->deleteOldCheckpoints(this);

                if (ctx->hardShutdown)
                    break;

                if (ctx->softShutdown && ctx->replicatorFinished)
                    break;

                trackConfigFile();

                {
                    if (unlikely(ctx->trace & Ctx::TRACE::SLEEP))
                        ctx->logTrace(Ctx::TRACE::SLEEP, "Checkpoint:run lastCheckpointScn: " + std::to_string(metadata->lastCheckpointScn) +
                                                         " checkpointScn: " + std::to_string(metadata->checkpointScn));

                    contextSet(Thread::CONTEXT_MUTEX, Thread::CHECKPOINT_RUN);
                    std::unique_lock<std::mutex> lck(mtx);
                    contextSet(Thread::CONTEXT_WAIT, CHECKPOINT_NO_WORK);
                    condLoop.wait_for(lck, std::chrono::milliseconds(100));
                }
                contextSet(Thread::CONTEXT_CPU);
            }

            if (ctx->softShutdown)
                metadata->writeCheckpoint(this, true);

        } catch (RuntimeException& ex) {
            ctx->error(ex.code, ex.msg);
            ctx->stopHard();
        } catch (std::bad_alloc& ex) {
            ctx->error(10018, "memory allocation failed: " + std::string(ex.what()));
            ctx->stopHard();
        }

        if (unlikely(ctx->trace & Ctx::TRACE::THREADS)) {
            std::ostringstream ss;
            ss << std::this_thread::get_id();
            ctx->logTrace(Ctx::TRACE::THREADS, "checkpoint (" + ss.str() + ") stop");
        }
    }
}
