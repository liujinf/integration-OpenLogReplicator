/* Context of program
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

#define GLOBALS 1

#include <cstdlib>
#include <csignal>
#include <execinfo.h>
#include <iostream>
#include <set>
#include <string>
#include <unistd.h>

#include "ClockHW.h"
#include "Ctx.h"
#include "Thread.h"
#include "typeIntX.h"
#include "exception/DataException.h"
#include "exception/RuntimeException.h"
#include "metrics/Metrics.h"

uint OLR_LOCALES = OpenLogReplicator::Ctx::LOCALES::TIMESTAMP;

namespace OpenLogReplicator {
    const char Ctx::map64[65]{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

    const char Ctx::map64R[256]{
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0, 63,
            52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0, 0, 0, 0, 0, 0,
            0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
            15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0, 0, 0, 0, 0,
            0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
            41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    const std::string Ctx::memoryModules[MEMORY_COUNT]{"builder", "parser", "reader", "transaction"};

    const int64_t Ctx::cumDays[12]{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const int64_t Ctx::cumDaysLeap[12]{0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};

    typeIntX typeIntX::BASE10[typeIntX::DIGITS][10];

    Ctx::Ctx() :
            bigEndian(false),
            memoryChunks(nullptr),
            memoryChunksMin(0),
            memoryChunksMax(0),
            memoryChunksSwap(0),
            memoryChunksAllocated(0),
            memoryChunksFree(0),
            memoryChunksHWM(0),
            memoryModulesAllocated{0, 0, 0, 0},
            outOfMemoryParser(false),
            mainThread(pthread_self()),
            memoryModulesHWM{0, 0, 0, 0},
            metrics(nullptr),
            clock(nullptr),
            version12(false),
            version(0),
            columnLimit(COLUMN_LIMIT),
            dumpRedoLog(0),
            dumpRawData(0),
            dumpStream(std::make_unique<std::ofstream>()),

            memoryChunksReadBufferMax(0),
            memoryChunksReadBufferMin(0),
            memoryChunksUnswapBufferMin(0),
            memoryChunksWriteBufferMin(0),

            bufferSizeMax(0),
            bufferSizeFree(0),
            bufferSizeHWM(0),
            suppLogSize(0),
            checkpointIntervalS(600),
            checkpointIntervalMb(500),
            checkpointKeep(100),
            schemaForceInterval(20),
            redoReadSleepUs(50000),
            redoVerifyDelayUs(0),
            archReadSleepUs(10000000),
            archReadTries(10),
            refreshIntervalUs(10000000),
            pollIntervalUs(100000),
            queueSize(65536),
            dumpPath("."),
            stopLogSwitches(0),
            stopCheckpoints(0),
            stopTransactions(0),
            transactionSizeMax(0),
            logLevel(LOG::INFO),
            trace(0),
            flags(0),
            disableChecks(0),
            hardShutdown(false),
            softShutdown(false),
            replicatorFinished(false),
            parserThread(nullptr),
            writerThread(nullptr),
            swappedMB(0),
            swappedFlushXid(0, 0, 0),
            swappedShrinkXid(0, 0, 0) {
        clock = new ClockHW();
        tzset();
        dbTimezone = BAD_TIMEZONE;
        logTimezone = -timezone;
        hostTimezone = -timezone;
    }

    Ctx::~Ctx() {
        lobIdToXidMap.clear();

        while (memoryChunksAllocated > 0) {
            --memoryChunksAllocated;
            free(memoryChunks[memoryChunksAllocated]);
            memoryChunks[memoryChunksAllocated] = nullptr;
        }

        if (memoryChunks != nullptr) {
            delete[] memoryChunks;
            memoryChunks = nullptr;
        }

        if (metrics != nullptr) {
            metrics->shutdown();
            delete metrics;
            metrics = nullptr;
        }

        if (clock != nullptr) {
            delete clock;
            clock = nullptr;
        }
    }

    void Ctx::checkJsonFields(const std::string& fileName, const rapidjson::Value& value, const char* names[]) {
        for (auto const& child: value.GetObject()) {
            bool found = false;
            for (int i = 0; names[i] != nullptr; ++i) {
                if (strcmp(child.name.GetString(), names[i]) == 0) {
                    found = true;
                    break;
                }
            }

            if (unlikely(!found && memcmp(child.name.GetString(), "xdb-xnm", 7) != 0 &&
                         memcmp(child.name.GetString(), "xdb-xpt", 7) != 0 &&
                         memcmp(child.name.GetString(), "xdb-xqn", 7) != 0))
                throw DataException(20003, "file: " + fileName + " - parse error, attribute " + child.name.GetString() + " not expected");
        }
    }

    const rapidjson::Value& Ctx::getJsonFieldA(const std::string& fileName, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsArray()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not an array");
        return ret;
    }

    uint16_t Ctx::getJsonFieldU16(const std::string& fileName, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsUint64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not an unsigned 64-bit number");
        uint64_t val = ret.GetUint64();
        if (unlikely(val > 0xFFFF))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is too big (" + std::to_string(val) + ")");
        return val;
    }

    int16_t Ctx::getJsonFieldI16(const std::string& fileName, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsInt64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not a signed 64-bit number");
        int64_t val = ret.GetInt64();
        if (unlikely((val > static_cast<int64_t>(0x7FFF)) || (val < -static_cast<int64_t>(0x8000))))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is too big (" + std::to_string(val) + ")");
        return static_cast<int16_t>(val);
    }

    uint32_t Ctx::getJsonFieldU32(const std::string& fileName, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsUint64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not an unsigned 64-bit number");
        uint64_t val = ret.GetUint64();
        if (unlikely(val > 0xFFFFFFFF))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is too big (" + std::to_string(val) + ")");
        return static_cast<uint32_t>(val);
    }

    int32_t Ctx::getJsonFieldI32(const std::string& fileName, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsInt64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not a signed 64-bit number");
        int64_t val = ret.GetInt64();
        if (unlikely((val > static_cast<int64_t>(0x7FFFFFFF)) || (val < -static_cast<int64_t>(0x80000000))))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is too big (" + std::to_string(val) + ")");
        return static_cast<int32_t>(val);
    }

    uint64_t Ctx::getJsonFieldU64(const std::string& fileName, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsUint64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not an unsigned 64-bit number");
        return ret.GetUint64();
    }

    int64_t Ctx::getJsonFieldI64(const std::string& fileName, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsInt64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not a signed 64-bit number");
        return ret.GetInt64();
    }

    uint Ctx::getJsonFieldU(const std::string& fileName, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsUint()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not an unsigned number");
        return ret.GetUint();
    }

    int Ctx::getJsonFieldI(const std::string& fileName, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsInt()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not a signed number");
        return ret.GetInt();
    }

    const rapidjson::Value& Ctx::getJsonFieldO(const std::string& fileName, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsObject()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not an object");
        return ret;
    }

    const char* Ctx::getJsonFieldS(const std::string& fileName, uint maxLength, const rapidjson::Value& value, const char* field) {
        if (unlikely(!value.HasMember(field)))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " not found");
        const rapidjson::Value& ret = value[field];
        if (unlikely(!ret.IsString()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is not a string");
        if (unlikely(ret.GetStringLength() > maxLength))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + " is too long (" +
                                       std::to_string(ret.GetStringLength()) + ", max: " + std::to_string(maxLength) + ")");
        return ret.GetString();
    }

    const rapidjson::Value& Ctx::getJsonFieldA(const std::string& fileName, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsArray()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not an array");
        return ret;
    }

    uint16_t Ctx::getJsonFieldU16(const std::string& fileName, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsUint64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not an unsigned 64-bit number");
        uint64_t val = ret.GetUint64();
        if (unlikely(val > 0xFFFF))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is too big (" + std::to_string(val) + ")");
        return val;
    }

    int16_t Ctx::getJsonFieldI16(const std::string& fileName, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsInt64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not a signed 64-bit number");
        int64_t val = ret.GetInt64();
        if (unlikely((val > static_cast<int64_t>(0x7FFF)) || (val < -static_cast<int64_t>(0x8000))))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is too big (" + std::to_string(val) + ")");
        return static_cast<int16_t>(val);
    }

    uint32_t Ctx::getJsonFieldU32(const std::string& fileName, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsUint64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not an unsigned 64-bit number");
        uint64_t val = ret.GetUint64();
        if (unlikely(val > 0xFFFFFFFF))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is too big (" + std::to_string(val) + ")");
        return static_cast<uint32_t>(val);
    }

    int32_t Ctx::getJsonFieldI32(const std::string& fileName, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsInt64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not a signed 64-bit number");
        int64_t val = ret.GetInt64();
        if (unlikely((val > static_cast<int64_t>(0x7FFFFFFF)) || (val < -static_cast<int64_t>(0x80000000))))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is too big (" + std::to_string(val) + ")");
        return static_cast<int32_t>(val);
    }

    uint64_t Ctx::getJsonFieldU64(const std::string& fileName, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsUint64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not an unsigned 64-bit number");
        return ret.GetUint64();
    }

    int64_t Ctx::getJsonFieldI64(const std::string& fileName, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsInt64()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not a signed 64-bit number");
        return ret.GetInt64();
    }

    uint Ctx::getJsonFieldU(const std::string& fileName, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsUint()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not an unsigned number");
        return ret.GetUint();
    }

    int Ctx::getJsonFieldI(const std::string& fileName, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsInt()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not a signed number");
        return ret.GetInt();
    }

    const rapidjson::Value& Ctx::getJsonFieldO(const std::string& fileName, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsObject()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not an object");
        return ret;
    }

    const char* Ctx::getJsonFieldS(const std::string& fileName, uint maxLength, const rapidjson::Value& value, const char* field, uint num) {
        const rapidjson::Value& ret = value[num];
        if (unlikely(!ret.IsString()))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is not a string");
        if (unlikely(ret.GetStringLength() > maxLength))
            throw DataException(20003, "file: " + fileName + " - parse error, field " + field + "[" + std::to_string(num) +
                                       "] is too long (" + std::to_string(ret.GetStringLength()) + ", max: " + std::to_string(maxLength) + ")");
        return ret.GetString();
    }

    bool Ctx::parseTimezone(const char* str, int64_t& out) const {
        if (strcmp(str, "Etc/GMT-14") == 0) str = "-14:00";
        if (strcmp(str, "Etc/GMT-13") == 0) str = "-13:00";
        if (strcmp(str, "Etc/GMT-12") == 0) str = "-12:00";
        if (strcmp(str, "Etc/GMT-11") == 0) str = "-11:00";
        if (strcmp(str, "HST") == 0) str = "-10:00";
        if (strcmp(str, "Etc/GMT-10") == 0) str = "-10:00";
        if (strcmp(str, "Etc/GMT-9") == 0) str = "-09:00";
        if (strcmp(str, "PST") == 0) str = "-08:00";
        if (strcmp(str, "PST8PDT") == 0) str = "-08:00";
        if (strcmp(str, "Etc/GMT-8") == 0) str = "-08:00";
        if (strcmp(str, "MST") == 0) str = "-07:00";
        if (strcmp(str, "MST7MDT") == 0) str = "-07:00";
        if (strcmp(str, "Etc/GMT-7") == 0) str = "-07:00";
        if (strcmp(str, "CST") == 0) str = "-06:00";
        if (strcmp(str, "CST6CDT") == 0) str = "-06:00";
        if (strcmp(str, "Etc/GMT-6") == 0) str = "-06:00";
        if (strcmp(str, "EST") == 0) str = "-05:00";
        if (strcmp(str, "EST5EDT") == 0) str = "-05:00";
        if (strcmp(str, "Etc/GMT-5") == 0) str = "-05:00";
        if (strcmp(str, "Etc/GMT-4") == 0) str = "-04:00";
        if (strcmp(str, "Etc/GMT-3") == 0) str = "-03:00";
        if (strcmp(str, "Etc/GMT-2") == 0) str = "-02:00";
        if (strcmp(str, "Etc/GMT-1") == 0) str = "-01:00";
        if (strcmp(str, "GMT") == 0) str = "+00:00";
        if (strcmp(str, "Etc/GMT") == 0) str = "+00:00";
        if (strcmp(str, "Greenwich") == 0) str = "+00:00";
        if (strcmp(str, "Etc/Greenwich") == 0) str = "+00:00";
        if (strcmp(str, "GMT0") == 0) str = "+00:00";
        if (strcmp(str, "Etc/GMT0") == 0) str = "+00:00";
        if (strcmp(str, "GMT+0") == 0) str = "+00:00";
        if (strcmp(str, "Etc/GMT-0") == 0) str = "+00:00";
        if (strcmp(str, "GMT+0") == 0) str = "+00:00";
        if (strcmp(str, "Etc/GMT+0") == 0) str = "+00:00";
        if (strcmp(str, "UTC") == 0) str = "+00:00";
        if (strcmp(str, "Etc/UTC") == 0) str = "+00:00";
        if (strcmp(str, "UCT") == 0) str = "+00:00";
        if (strcmp(str, "Etc/UCT") == 0) str = "+00:00";
        if (strcmp(str, "Universal") == 0) str = "+00:00";
        if (strcmp(str, "Etc/Universal") == 0) str = "+00:00";
        if (strcmp(str, "WET") == 0) str = "+00:00";
        if (strcmp(str, "MET") == 0) str = "+01:00";
        if (strcmp(str, "CET") == 0) str = "+01:00";
        if (strcmp(str, "Etc/GMT+1") == 0) str = "+01:00";
        if (strcmp(str, "EET") == 0) str = "+02:00";
        if (strcmp(str, "Etc/GMT+2") == 0) str = "+02:00";
        if (strcmp(str, "Etc/GMT+3") == 0) str = "+03:00";
        if (strcmp(str, "Etc/GMT+4") == 0) str = "+04:00";
        if (strcmp(str, "Etc/GMT+5") == 0) str = "+05:00";
        if (strcmp(str, "Etc/GMT+6") == 0) str = "+06:00";
        if (strcmp(str, "Etc/GMT+7") == 0) str = "+07:00";
        if (strcmp(str, "PRC") == 0) str = "+08:00";
        if (strcmp(str, "ROC") == 0) str = "+08:00";
        if (strcmp(str, "Etc/GMT+8") == 0) str = "+08:00";
        if (strcmp(str, "Etc/GMT+9") == 0) str = "+09:00";
        if (strcmp(str, "Etc/GMT+10") == 0) str = "+10:00";
        if (strcmp(str, "Etc/GMT+11") == 0) str = "+11:00";
        if (strcmp(str, "Etc/GMT+12") == 0) str = "+12:00";

        uint64_t len = strlen(str);

        if (len == 5) {
            if (str[1] >= '0' && str[1] <= '9' &&
                str[2] == ':' &&
                str[3] >= '0' && str[3] <= '9' &&
                str[4] >= '0' && str[4] <= '9') {
                out = -(str[1] - '0') * 3600 + (str[3] - '0') * 60 + (str[4] - '0');
            } else
                return false;
        } else if (len == 6) {
            if (str[1] >= '0' && str[1] <= '9' &&
                str[2] >= '0' && str[2] <= '9' &&
                str[3] == ':' &&
                str[4] >= '0' && str[4] <= '9' &&
                str[5] >= '0' && str[5] <= '9') {
                out = -(str[1] - '0') * 36000 + (str[2] - '0') * 3600 + (str[4] - '0') * 60 + (str[5] - '0');
            } else
                return false;
        } else
            return false;

        if (str[0] == '-')
            out = -out;
        else if (str[0] != '+')
            return false;

        return true;
    }

    std::string Ctx::timezoneToString(int64_t tz) const {
        char result[7];

        if (tz < 0) {
            result[0] = '-';
            tz = -tz;
        } else
            result[0] = '+';

        tz /= 60;

        result[6] = 0;
        result[5] = map10(tz % 10);
        tz /= 10;
        result[4] = map10(tz % 6);
        tz /= 6;
        result[3] = ':';
        result[2] = map10(tz % 10);
        tz /= 10;
        result[1] = map10(tz % 10);

        return result;
    }

    time_t Ctx::valuesToEpoch(int year, int month, int day, int hour, int minute, int second, int tz) const {
        time_t result;

        if (year > 0) {
            result = yearToDays(year, month) + cumDays[month % 12] + day;
            result *= 24;
            result += hour;
            result *= 60;
            result += minute;
            result *= 60;
            result += second;
            return result - UNIX_AD1970_01_01 - tz; // adjust to 1970 epoch, 719527 days
        } else {
            // treat dates BC with the exact rules as AD for leap years
            result = -yearToDaysBC(-year, month) + cumDays[month % 12] + day;
            result *= 24;
            result += hour;
            result *= 60;
            result += minute;
            result *= 60;
            result += second;
            return result - UNIX_BC1970_01_01 - tz; // adjust to 1970 epoch, 718798 days (year 0 does not exist)
        }
    }

    uint64_t Ctx::epochToIso8601(time_t timestamp, char* buffer, bool addT, bool addZ) const {
        // (-)YYYY-MM-DD hh:mm:ss or (-)YYYY-MM-DDThh:mm:ssZ

        if (unlikely(timestamp < UNIX_BC4712_01_01 || timestamp > UNIX_AD9999_12_31))
            throw RuntimeException(10069, "invalid timestamp value: " + std::to_string(timestamp));

        timestamp += UNIX_AD1970_01_01;
        if (timestamp >= 365 * 60 * 60 * 24) {
            // AD
            int64_t second = (timestamp % 60);
            timestamp /= 60;
            int64_t minute = (timestamp % 60);
            timestamp /= 60;
            int64_t hour = (timestamp % 24);
            timestamp /= 24;

            int64_t year = timestamp / 365 + 1;
            int64_t day = yearToDays(year, 0);

            while (day > timestamp) {
                --year;
                day = yearToDays(year, 0);
            }
            day = timestamp - day;

            int64_t month = day / 27;
            if (month > 11) month = 11;

            if ((year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0)) {
                // leap year
                while (cumDaysLeap[month] > day)
                    --month;
                day -= cumDaysLeap[month];
            } else {
                while (cumDays[month] > day)
                    --month;
                day -= cumDays[month];
            }
            ++month;
            ++day;

            buffer[3] = map10(year % 10);
            year /= 10;
            buffer[2] = map10(year % 10);
            year /= 10;
            buffer[1] = map10(year % 10);
            year /= 10;
            buffer[0] = map10(year);
            buffer[4] = '-';
            buffer[6] = map10(month % 10);
            month /= 10;
            buffer[5] = map10(month);
            buffer[7] = '-';
            buffer[9] = map10(day % 10);
            day /= 10;
            buffer[8] = map10(day);
            if (addT)
                buffer[10] = 'T';
            else
                buffer[10] = ' ';
            buffer[12] = map10(hour % 10);
            hour /= 10;
            buffer[11] = map10(hour);
            buffer[13] = ':';
            buffer[15] = map10(minute % 10);
            minute /= 10;
            buffer[14] = map10(minute);
            buffer[16] = ':';
            buffer[18] = map10(second % 10);
            second /= 10;
            buffer[17] = map10(second);
            if (addZ) {
                buffer[19] = 'Z';
                buffer[20] = 0;
                return 20;
            } else {
                buffer[19] = 0;
                return 19;
            }
        } else {
            // BC
            timestamp = 365 * 24 * 60 * 60 - timestamp;

            int64_t second = (timestamp % 60);
            timestamp /= 60;
            int64_t minute = (timestamp % 60);
            timestamp /= 60;
            int64_t hour = (timestamp % 24);
            timestamp /= 24;

            int64_t year = timestamp / 366 - 1;
            if (year < 0) year = 0;
            int64_t day = yearToDaysBC(year, 0);

            while (day < timestamp) {
                ++year;
                day = yearToDaysBC(year, 0);
            }
            day -= timestamp;

            int64_t month = day / 27;
            if (month > 11) month = 11;

            if ((year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0)) {
                // leap year
                while (cumDaysLeap[month] > day)
                    --month;
                day -= cumDaysLeap[month];
            } else {
                while (cumDays[month] > day)
                    --month;
                day -= cumDays[month];
            }
            ++month;
            ++day;
            buffer[0] = '-';
            buffer[4] = map10(year % 10);
            year /= 10;
            buffer[3] = map10(year % 10);
            year /= 10;
            buffer[2] = map10(year % 10);
            year /= 10;
            buffer[1] = map10(year);
            buffer[5] = '-';
            buffer[7] = map10(month % 10);
            month /= 10;
            buffer[6] = map10(month);
            buffer[8] = '-';
            buffer[10] = map10(day % 10);
            day /= 10;
            buffer[9] = map10(day);
            if (addT)
                buffer[11] = 'T';
            else
                buffer[11] = ' ';
            buffer[13] = map10(hour % 10);
            hour /= 10;
            buffer[12] = map10(hour);
            buffer[14] = ':';
            buffer[16] = map10(minute % 10);
            minute /= 10;
            buffer[15] = map10(minute);
            buffer[17] = ':';
            buffer[19] = map10(second % 10);
            second /= 10;
            buffer[18] = map10(second);
            if (addZ) {
                buffer[20] = 'Z';
                buffer[21] = 0;
                return 21;
            } else {
                buffer[20] = 0;
                return 20;
            }
        };
    }

    void Ctx::initialize(uint64_t memoryMinMb, uint64_t memoryMaxMb, uint64_t memoryReadBufferMaxMb, uint64_t memoryReadBufferMinMb, uint64_t memorySwapMb,
                         uint64_t memoryUnswapBufferMinMb, uint64_t memoryWriteBufferMaxMb, uint64_t memoryWriteBufferMinMb) {
        {
            std::unique_lock<std::mutex> lck(memoryMtx);
            memoryChunksMin = memoryMinMb / MEMORY_CHUNK_SIZE_MB;
            memoryChunksMax = memoryMaxMb / MEMORY_CHUNK_SIZE_MB;
            memoryChunksSwap = memorySwapMb / MEMORY_CHUNK_SIZE_MB;

            memoryChunksReadBufferMax = memoryReadBufferMaxMb / MEMORY_CHUNK_SIZE_MB;
            memoryChunksReadBufferMin = memoryReadBufferMinMb / MEMORY_CHUNK_SIZE_MB;
            memoryChunksUnswapBufferMin = memoryUnswapBufferMinMb / MEMORY_CHUNK_SIZE_MB;
            memoryChunksWriteBufferMax = memoryWriteBufferMaxMb / MEMORY_CHUNK_SIZE_MB;
            memoryChunksWriteBufferMin = memoryWriteBufferMinMb / MEMORY_CHUNK_SIZE_MB;
            bufferSizeMax = memoryReadBufferMaxMb * 1024 * 1024;
            bufferSizeFree = memoryReadBufferMaxMb / MEMORY_CHUNK_SIZE_MB;

            memoryChunks = new uint8_t* [memoryChunksMax];
            for (uint64_t i = 0; i < memoryChunksMin; ++i) {
                memoryChunks[i] = reinterpret_cast<uint8_t*>(aligned_alloc(MEMORY_ALIGNMENT, MEMORY_CHUNK_SIZE));
                if (unlikely(memoryChunks[i] == nullptr))
                    throw RuntimeException(10016, "couldn't allocate " + std::to_string(MEMORY_CHUNK_SIZE_MB) +
                                                  " bytes memory for: memory chunks#2");
                ++memoryChunksAllocated;
                ++memoryChunksFree;
            }
            memoryChunksHWM = memoryChunksMin;
        }

        if (metrics) {
            metrics->emitMemoryAllocatedMb(memoryChunksAllocated);
            metrics->emitMemoryUsedTotalMb(0);
        }
    }

    void Ctx::wakeAllOutOfMemory() {
        std::unique_lock<std::mutex> lck(memoryMtx);
        condOutOfMemory.notify_all();
    }

    bool Ctx::nothingToSwap(Thread* t) const {
        bool ret;
        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_NOTHING_TO_SWAP);
            std::unique_lock<std::mutex> lck(memoryMtx);
            ret = memoryChunksSwap == 0 || (memoryChunksAllocated - memoryChunksFree < memoryChunksSwap);
        }
        t->contextSet(Thread::CONTEXT_CPU);
        return ret;
    }

    uint64_t Ctx::getMemoryHWM() const {
        std::unique_lock<std::mutex> lck(memoryMtx);
        return memoryChunksHWM * MEMORY_CHUNK_SIZE_MB;
    }

    uint64_t Ctx::getFreeMemory(Thread* t) const {
        uint64_t ret;
        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_FREE_MEMORY);
            std::unique_lock<std::mutex> lck(memoryMtx);
            ret = memoryChunksFree * MEMORY_CHUNK_SIZE_MB;
        }
        t->contextSet(Thread::CONTEXT_CPU);
        return ret;
    }

    uint64_t Ctx::getAllocatedMemory() const {
        std::unique_lock<std::mutex> lck(memoryMtx);
        return memoryChunksAllocated * MEMORY_CHUNK_SIZE_MB;
    }

    uint64_t Ctx::getSwapMemory(Thread* t) const {
        uint64_t ret;
        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_GET_SWAP);
            std::unique_lock<std::mutex> lck(memoryMtx);
            ret = memoryChunksSwap * MEMORY_CHUNK_SIZE_MB;
        }
        t->contextSet(Thread::CONTEXT_CPU);
        return ret;
    }

    uint8_t* Ctx::getMemoryChunk(Thread* t, MEMORY module, bool swap) {
        uint64_t allocatedModule = 0, usedTotal = 0, allocatedTotal = 0;
        uint8_t* chunk = nullptr;

        t->contextSet(Thread::CONTEXT_MEM, Thread::REASON_MEM);
        {
            std::unique_lock<std::mutex> lck(memoryMtx);
            while (true) {
                if (module == MEMORY::READER) {
                    if (memoryModulesAllocated[MEMORY::READER] < memoryChunksReadBufferMin)
                        break;
                } else if (module == MEMORY::BUILDER) {
                    if (memoryModulesAllocated[MEMORY::BUILDER] < memoryChunksWriteBufferMin)
                        break;
                }

                uint64_t reservedChunks = 0;
                if (memoryModulesAllocated[MEMORY::READER] < memoryChunksReadBufferMin)
                    reservedChunks += memoryChunksReadBufferMin - memoryModulesAllocated[MEMORY::READER];
                if (memoryModulesAllocated[MEMORY::BUILDER] < memoryChunksWriteBufferMin)
                    reservedChunks += memoryChunksWriteBufferMin - memoryModulesAllocated[MEMORY::BUILDER];
                if (!swap)
                    reservedChunks += memoryChunksUnswapBufferMin;

                if (module != MEMORY::BUILDER || memoryModulesAllocated[MEMORY::BUILDER] < memoryChunksWriteBufferMax) {
                    if (memoryChunksFree > reservedChunks)
                        break;

                    if (memoryChunksAllocated < memoryChunksMax) {
                        t->contextSet(Thread::CONTEXT_OS, Thread::REASON_OS);
                        memoryChunks[memoryChunksFree] = reinterpret_cast<uint8_t*>(aligned_alloc(MEMORY_ALIGNMENT, MEMORY_CHUNK_SIZE));
                        t->contextSet(Thread::CONTEXT_MEM, Thread::REASON_MEM);
                        if (unlikely(memoryChunks[memoryChunksFree] == nullptr))
                            throw RuntimeException(10016, "couldn't allocate " + std::to_string(MEMORY_CHUNK_SIZE_MB) +
                                                          " bytes memory for: " + memoryModules[module]);
                        ++memoryChunksFree;
                        allocatedTotal = ++memoryChunksAllocated;

                        if (memoryChunksAllocated > memoryChunksHWM)
                            memoryChunksHWM = memoryChunksAllocated;
                        break;
                    }
                }

                if (module == MEMORY::PARSER)
                    outOfMemoryParser = true;

                if (hardShutdown)
                    return nullptr;

                if (unlikely(trace & TRACE::SLEEP))
                    logTrace(TRACE::SLEEP, "Ctx:getMemoryChunk");
                t->contextSet(Thread::CONTEXT_WAIT, Thread::MEMORY_EXHAUSTED);
                condOutOfMemory.wait(lck);
                t->contextSet(Thread::CONTEXT_MEM, Thread::REASON_MEM);
            }

            if (module == MEMORY::PARSER)
                outOfMemoryParser = false;

            --memoryChunksFree;
            usedTotal = memoryChunksAllocated - memoryChunksFree;
            allocatedModule = ++memoryModulesAllocated[module];
            if (memoryModulesAllocated[module] > memoryModulesHWM[module])
                memoryModulesHWM[module] = memoryModulesAllocated[module];
            chunk = memoryChunks[memoryChunksFree];
        }
        t->contextSet(Thread::CONTEXT_CPU);

        if (unlikely(hardShutdown))
            throw RuntimeException(10018, "shutdown during memory allocation");

        if (metrics) {
            if (allocatedTotal > 0)
                metrics->emitMemoryAllocatedMb(allocatedTotal * MEMORY_CHUNK_SIZE_MB);

            metrics->emitMemoryUsedTotalMb(usedTotal * MEMORY_CHUNK_SIZE_MB);

            switch (module) {
                case MEMORY::BUILDER:
                    metrics->emitMemoryUsedMbBuilder(allocatedModule * MEMORY_CHUNK_SIZE_MB);
                    break;

                case MEMORY::PARSER:
                    metrics->emitMemoryUsedMbParser(allocatedModule * MEMORY_CHUNK_SIZE_MB);
                    break;

                case MEMORY::READER:
                    metrics->emitMemoryUsedMbReader(allocatedModule * MEMORY_CHUNK_SIZE_MB);
                    break;

                case MEMORY::TRANSACTIONS:
                    metrics->emitMemoryUsedMbTransactions(allocatedModule * MEMORY_CHUNK_SIZE_MB);
            }
        }

        return chunk;
    }

    void Ctx::freeMemoryChunk(Thread* t, MEMORY module, uint8_t* chunk) {
        uint64_t allocatedModule = 0, usedTotal = 0, allocatedTotal = 0;
        t->contextSet(Thread::CONTEXT_MEM, Thread::REASON_MEM);
        {
            std::unique_lock<std::mutex> lck(memoryMtx);

            if (unlikely(memoryChunksFree == memoryChunksAllocated))
                throw RuntimeException(50001, "trying to free unknown memory block for: " + memoryModules[module]);

            // Keep memoryChunksMin reserved
            if (memoryChunksFree >= memoryChunksMin)
                allocatedTotal = --memoryChunksAllocated;
            else {
                memoryChunks[memoryChunksFree++] = chunk;
                chunk = nullptr;
            }

            usedTotal = memoryChunksAllocated - memoryChunksFree;
            allocatedModule = --memoryModulesAllocated[module];

            condOutOfMemory.notify_all();
        }

        if (chunk != nullptr) {
            t->contextSet(Thread::CONTEXT_OS, Thread::REASON_OS);
            free(chunk);
        }

        t->contextSet(Thread::CONTEXT_CPU);
        if (metrics) {
            if (allocatedTotal > 0)
                metrics->emitMemoryAllocatedMb(allocatedTotal * MEMORY_CHUNK_SIZE_MB);

            metrics->emitMemoryUsedTotalMb(usedTotal * MEMORY_CHUNK_SIZE_MB);

            switch (module) {
                case MEMORY::BUILDER:
                    metrics->emitMemoryUsedMbBuilder(allocatedModule * MEMORY_CHUNK_SIZE_MB);
                    break;

                case MEMORY::PARSER:
                    metrics->emitMemoryUsedMbParser(allocatedModule * MEMORY_CHUNK_SIZE_MB);
                    break;

                case MEMORY::READER:
                    metrics->emitMemoryUsedMbReader(allocatedModule * MEMORY_CHUNK_SIZE_MB);
                    break;

                case MEMORY::TRANSACTIONS:
                    metrics->emitMemoryUsedMbTransactions(allocatedModule * MEMORY_CHUNK_SIZE_MB);
            }
        }
    }

    void Ctx::swappedMemoryInit(Thread* t, typeXid xid) {
        SwapChunk* sc = new SwapChunk();
        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_MEMORY_INIT);
            std::unique_lock<std::mutex> lck(swapMtx);
            swapChunks.insert_or_assign(xid, sc);
        }
        t->contextSet(Thread::CONTEXT_CPU);
    }

    uint64_t Ctx::swappedMemorySize(Thread* t, typeXid xid) const {
        uint64_t ret;
        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_SWAPPED_SIZE);
            std::unique_lock<std::mutex> lck(swapMtx);
            auto it = swapChunks.find(xid);
            if (unlikely(it == swapChunks.end()))
                throw RuntimeException(50070, "swap chunk not found for xid: " + xid.toString() + " during memory size");
            SwapChunk* sc = it->second;
            ret = sc->chunks.size();
        }
        t->contextSet(Thread::CONTEXT_CPU);
        return ret;
    }

    uint8_t* Ctx::swappedMemoryGet(Thread* t, typeXid xid, int64_t index) {
        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_SWAPPED_GET);
            std::unique_lock<std::mutex> lck(swapMtx);
            auto it = swapChunks.find(xid);
            if (unlikely(it == swapChunks.end()))
                throw RuntimeException(50070, "swap chunk not found for xid: " + xid.toString() + " during memory get");
            SwapChunk* sc = it->second;

            while (!hardShutdown) {
                if (index < sc->swappedMin || index > sc->swappedMax) {
                    t->contextSet(Thread::CONTEXT_CPU);
                    return sc->chunks.at(index);
                }

                chunksMemoryManager.notify_all();
                chunksTransaction.wait(lck);
            }
        }

        t->contextSet(Thread::CONTEXT_CPU);
        return nullptr;
    }

    void Ctx::swappedMemoryRelease(Thread* t, typeXid xid, int64_t index) {
        uint8_t* tc;
        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_SWAPPED_RELEASE);
            std::unique_lock<std::mutex> lck(swapMtx);
            auto it = swapChunks.find(xid);
            if (unlikely(it == swapChunks.end()))
                throw RuntimeException(50070, "swap chunk not found for xid: " + xid.toString() + " during memory release");
            SwapChunk* sc = it->second;
            tc = sc->chunks.at(index);
            sc->chunks[index] = nullptr;
        }
        t->contextSet(Thread::CONTEXT_CPU);

        freeMemoryChunk(t, Ctx::MEMORY::TRANSACTIONS, tc);
    }

    [[nodiscard]] uint8_t* Ctx::swappedMemoryGrow(Thread* t, typeXid xid) {
        SwapChunk* sc;
        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_SWAPPED_GROW1);
            std::unique_lock<std::mutex> lck(swapMtx);
            auto it = swapChunks.find(xid);
            if (unlikely(it == swapChunks.end()))
                throw RuntimeException(50070, "swap chunk not found for xid: " + xid.toString() + " during memory grow");
            sc = it->second;
        }
        t->contextSet(Thread::CONTEXT_CPU);

        uint8_t* tc = getMemoryChunk(t, Ctx::MEMORY::TRANSACTIONS);
        memset(tc, 0, sizeof(uint64_t) + sizeof(uint32_t));

        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_SWAPPED_GROW2);
            std::unique_lock<std::mutex> lck(swapMtx);
            sc->chunks.push_back(tc);
        }
        t->contextSet(Thread::CONTEXT_CPU);
        return tc;
    }

    uint8_t* Ctx::swappedMemoryShrink(Thread* t, typeXid xid) {
        SwapChunk* sc;
        uint8_t* tc;
        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_SWAPPED_SHRINK1);
            std::unique_lock<std::mutex> lck(swapMtx);
            auto it = swapChunks.find(xid);
            if (unlikely(it == swapChunks.end()))
                throw RuntimeException(50070, "swap chunk not found for xid: " + xid.toString() + " during memory shrink");
            sc = it->second;
            tc = sc->chunks.back();
            sc->chunks.pop_back();
        }

        freeMemoryChunk(t, Ctx::MEMORY::TRANSACTIONS, tc);

        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_SWAPPED_SHRINK2);
            std::unique_lock<std::mutex> lck(swapMtx);
            if (sc->chunks.size() == 0) {
                t->contextSet(Thread::CONTEXT_CPU);
                return nullptr;
            }
            int64_t index = sc->chunks.size() - 1;

            swappedShrinkXid = xid;
            while (!hardShutdown) {
                if (index < sc->swappedMin || index > sc->swappedMax)
                    break;

                chunksMemoryManager.notify_all();
                chunksTransaction.wait(lck);
            }
            swappedShrinkXid = 0;
            tc = sc->chunks.back();
        }
        t->contextSet(Thread::CONTEXT_CPU);
        return tc;
    }

    void Ctx::swappedMemoryFlush(Thread* t, typeXid xid) {
        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_SWAPPED_FLUSH1);
            std::unique_lock<std::mutex> lck(swapMtx);
            swappedFlushXid = xid;
        }
        t->contextSet(Thread::CONTEXT_CPU);
    }

    void Ctx::swappedMemoryRemove(Thread* t, typeXid xid) {
        SwapChunk* sc;
        {
            t->contextSet(Thread::CONTEXT_CPU);
            std::unique_lock<std::mutex> lck(swapMtx);
            auto it = swapChunks.find(xid);
            if (unlikely(it == swapChunks.end()))
                throw RuntimeException(50070, "swap chunk not found for xid: " + xid.toString() + " during memory remove");
            sc = it->second;
            sc->release = true;
            swappedFlushXid = 0;
        }
        t->contextSet(Thread::CONTEXT_CPU);

        for (auto tc: sc->chunks)
            if (tc != nullptr)
                freeMemoryChunk(t, Ctx::MEMORY::TRANSACTIONS, tc);

        {
            t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_SWAPPED_FLUSH2);
            std::unique_lock<std::mutex> lck(swapMtx);
            sc->chunks.clear();
            commitedXids.push_back(xid);
            chunksMemoryManager.notify_all();
        }
        t->contextSet(Thread::CONTEXT_CPU);
    }

    void Ctx::wontSwap(Thread* t) {
        t->contextSet(Thread::CONTEXT_MUTEX, Thread::CTX_SWAPPED_WONT);
        std::unique_lock<std::mutex> lck(memoryMtx);

        if (!outOfMemoryParser) {
            t->contextSet(Thread::CONTEXT_CPU);
            return;
        }

        if (memoryModulesAllocated[MEMORY::BUILDER] > memoryChunksWriteBufferMin) {
            t->contextSet(Thread::CONTEXT_CPU);
            return;
        }

        hint("try to restart with higher value of 'memory-max-mb' parameter or if big transaction - add to 'skip-xid' list; "
             "transaction would be skipped");
        if (memoryModulesAllocated[MEMORY::READER] > 5)
            hint("amount of disk buffer is too high, try to decrease 'memory-read-buffer-max-mb' parameter, current utilization: " +
                 std::to_string(memoryModulesAllocated[MEMORY::READER] * MEMORY_CHUNK_SIZE_MB) + "MB");
        throw RuntimeException(10017, "out of memory");
    }

    void Ctx::stopHard() {
        logTrace(TRACE::THREADS, "stop hard");

        {
            std::unique_lock<std::mutex> lck(mtx);

            if (hardShutdown)
                return;
            hardShutdown = true;
            softShutdown = true;

            condMainLoop.notify_all();
        }
        {
            std::unique_lock<std::mutex> lck(memoryMtx);
            condOutOfMemory.notify_all();
        }
    }

    void Ctx::stopSoft() {
        logTrace(TRACE::THREADS, "stop soft");

        std::unique_lock<std::mutex> lck(mtx);
        if (softShutdown)
            return;

        softShutdown = true;
        condMainLoop.notify_all();
    }

    void Ctx::mainFinish() {
        logTrace(TRACE::THREADS, "main finish start");

        while (wakeThreads()) {
            usleep(10000);
            wakeAllOutOfMemory();
        }

        while (!threads.empty()) {
            Thread* thread;
            {
                std::unique_lock<std::mutex> lck(mtx);
                thread = *(threads.cbegin());
            }
            finishThread(thread);
        }

        logTrace(TRACE::THREADS, "main finish end");
    }

    void Ctx::mainLoop() {
        logTrace(TRACE::THREADS, "main loop start");

        {
            std::unique_lock<std::mutex> lck(mtx);
            if (!hardShutdown) {
                if (unlikely(trace & TRACE::SLEEP))
                    logTrace(TRACE::SLEEP, "Ctx:mainLoop");
                condMainLoop.wait(lck);
            }
        }

        logTrace(TRACE::THREADS, "main loop end");
    }

    void Ctx::printStacktrace() {
        void* array[128];
        int size;
        std::stringstream result;
        result << "stacktrace for thread: " + std::to_string(reinterpret_cast<uint64_t>(pthread_self())) + "\n";
        {
            std::unique_lock<std::mutex> lck(mtx);
            size = backtrace(array, 128);
        }
        char** ptr = backtrace_symbols(array, size);

        if (ptr == nullptr) {
            result << "empty";
            error(10014, result.str());
            return;
        }

        for (int i = 0; i < size; ++i)
            result << ptr[i] << "\n";

        free(ptr);

        error(10014, result.str());
    }

    void Ctx::signalHandler(int s) {
        if (!hardShutdown) {
            error(10015, "caught signal: " + std::to_string(s));
            stopHard();
        }
    }

    bool Ctx::wakeThreads() {
        logTrace(TRACE::THREADS, "wake threads");

        bool wakingUp = false;
        {
            std::unique_lock<std::mutex> lck(mtx);
            for (Thread* thread: threads) {
                if (!thread->finished) {
                    logTrace(TRACE::THREADS, "waking up thread: " + thread->alias);
                    thread->wakeUp();
                    wakingUp = true;
                }
            }
        }
        wakeAllOutOfMemory();

        return wakingUp;
    }

    void Ctx::spawnThread(Thread* t) {
        logTrace(TRACE::THREADS, "spawn: " + t->alias);

        if (unlikely(pthread_create(&t->pthread, nullptr, &Thread::runStatic, reinterpret_cast<void*>(t))))
            throw RuntimeException(10013, "spawning thread: " + t->alias);
        {
            std::unique_lock<std::mutex> lck(mtx);
            threads.insert(t);
        }
    }

    void Ctx::finishThread(Thread* t) {
        logTrace(TRACE::THREADS, "finish: " + t->alias);

        std::unique_lock<std::mutex> lck(mtx);
        if (threads.find(t) == threads.end())
            return;
        threads.erase(t);
        pthread_join(t->pthread, nullptr);
    }

    std::ostringstream& Ctx::writeEscapeValue(std::ostringstream& ss, const std::string& str) {
        const char* c_str = str.c_str();
        for (uint64_t i = 0; i < str.length(); ++i) {
            if (*c_str == '\t') {
                ss << "\\t";
            } else if (*c_str == '\r') {
                ss << "\\r";
            } else if (*c_str == '\n') {
                ss << "\\n";
            } else if (*c_str == '\b') {
                ss << "\\b";
            } else if (*c_str == '\f') {
                ss << "\\f";
            } else if (*c_str == '"' || *c_str == '\\') {
                ss << '\\' << *c_str;
            } else if (*c_str < 32) {
                ss << "\\u00" << Ctx::map16((*c_str >> 4) & 0x0F) << Ctx::map16(*c_str & 0x0F);
            } else {
                ss << *c_str;
            }
            ++c_str;
        }
        return ss;
    }

    bool Ctx::checkNameCase(const char* name) {
        uint64_t num = 0;
        while (*(name + num) != 0) {
            if (islower(static_cast<unsigned char>(*(name + num))))
                return false;

            if (unlikely(num == 1024))
                throw DataException(20004, "identifier '" + std::string(name) + "' is too long");
            ++num;
        }

        return true;
    }

    void Ctx::signalDump() {
        if (mainThread != pthread_self())
            return;

        std::unique_lock<std::mutex> lck(mtx);
        printMemoryUsageCurrent();
        for (Thread* thread: threads) {
            error(10014, "Dump: " + thread->getName() + " " + std::to_string(reinterpret_cast<uint64_t>(thread->pthread)) + " context: " +
                         std::to_string(thread->curContext) + " reason: " + std::to_string(thread->curReason) + " switches: " +
                         std::to_string(thread->contextSwitches));
            pthread_kill(thread->pthread, SIGUSR1);
        }
    }

    void Ctx::welcome(const std::string& message) const {
        int code = 0;
        if (OLR_LOCALES == LOCALES::TIMESTAMP) {
            std::ostringstream s;
            char timestamp[30];
            epochToIso8601(clock->getTimeT() + logTimezone, timestamp, false, false);
            s << timestamp << " INFO  " << std::setw(5) << std::setfill('0') << std::dec << code << " " << message << std::endl;
            std::cerr << s.str();
        } else {
            std::ostringstream s;
            s << " INFO  " << std::setw(5) << std::setfill('0') << std::dec << code << " " << message << std::endl;
            std::cerr << s.str();
        }
    }

    void Ctx::hint(const std::string& message) const {
        if (logLevel < LOG::ERROR)
            return;

        if (OLR_LOCALES == LOCALES::TIMESTAMP) {
            std::ostringstream s;
            char timestamp[30];
            epochToIso8601(clock->getTimeT() + logTimezone, timestamp, false, false);
            s << timestamp << " HINT  " << message << std::endl;
            std::cerr << s.str();
        } else {
            std::ostringstream s;
            s << "HINT  " << message << std::endl;
            std::cerr << s.str();
        }
    }

    void Ctx::error(int code, const std::string& message) const {
        if (logLevel < LOG::ERROR)
            return;

        if (OLR_LOCALES == LOCALES::TIMESTAMP) {
            std::ostringstream s;
            char timestamp[30];
            epochToIso8601(clock->getTimeT() + logTimezone, timestamp, false, false);
            s << timestamp << " ERROR " << std::setw(5) << std::setfill('0') << std::dec << code << " " << message << std::endl;
            std::cerr << s.str();
        } else {
            std::ostringstream s;
            s << "ERROR " << std::setw(5) << std::setfill('0') << std::dec << code << " " << message << std::endl;
            std::cerr << s.str();
        }
    }

    void Ctx::warning(int code, const std::string& message) const {
        if (logLevel < LOG::WARNING)
            return;

        if (OLR_LOCALES == LOCALES::TIMESTAMP) {
            std::ostringstream s;
            char timestamp[30];
            epochToIso8601(clock->getTimeT() + logTimezone, timestamp, false, false);
            s << timestamp << " WARN  " << std::setw(5) << std::setfill('0') << std::dec << code << " " << message << std::endl;
            std::cerr << s.str();
        } else {
            std::ostringstream s;
            s << "WARN  " << std::setw(5) << std::setfill('0') << std::dec << code << " " << message << std::endl;
            std::cerr << s.str();
        }
    }

    void Ctx::info(int code, const std::string& message) const {
        if (logLevel < LOG::INFO)
            return;

        if (OLR_LOCALES == LOCALES::TIMESTAMP) {
            std::ostringstream s;
            char timestamp[30];
            epochToIso8601(clock->getTimeT() + logTimezone, timestamp, false, false);
            s << timestamp << " INFO  " << std::setw(5) << std::setfill('0') << std::dec << code << " " << message << std::endl;
            std::cerr << s.str();
        } else {
            std::ostringstream s;
            s << "INFO  " << std::setw(5) << std::setfill('0') << std::dec << code << " " << message << std::endl;
            std::cerr << s.str();
        }
    }

    void Ctx::debug(int code, const std::string& message) const {
        if (logLevel < LOG::DEBUG)
            return;

        if (OLR_LOCALES == LOCALES::TIMESTAMP) {
            std::ostringstream s;
            char timestamp[30];
            epochToIso8601(clock->getTimeT() + logTimezone, timestamp, false, false);
            s << timestamp << " DEBUG " << std::setw(5) << std::setfill('0') << std::dec << code << " " << message << std::endl;
            std::cerr << s.str();
        } else {
            std::ostringstream s;
            s << "DEBUG " << std::setw(5) << std::setfill('0') << std::dec << code << " " << message << std::endl;
            std::cerr << s.str();
        }
    }

    void Ctx::logTrace(int mask, const std::string& message) const {
        const char* code = "XXXXX";
        if (likely((trace & mask) == 0))
            return;

        switch (mask) {
            case TRACE::DML:
                code = "DML  ";
                break;

            case TRACE::DUMP:
                code = "DUMP ";
                break;

            case TRACE::LOB:
                code = "LOB  ";
                break;

            case TRACE::LWN:
                code = "LWN  ";
                break;

            case TRACE::THREADS:
                code = "THRD ";
                break;

            case TRACE::SQL:
                code = "SQL  ";
                break;

            case TRACE::FILE:
                code = "FILE ";
                break;

            case TRACE::DISK:
                code = "DISK ";
                break;

            case TRACE::PERFORMANCE:
                code = "PERFM";
                break;

            case TRACE::TRANSACTION:
                code = "TRANX";
                break;

            case TRACE::REDO:
                code = "REDO ";
                break;

            case TRACE::ARCHIVE_LIST:
                code = "ARCHL";
                break;

            case TRACE::SCHEMA_LIST:
                code = "SCHEM";
                break;

            case TRACE::WRITER:
                code = "WRITR";
                break;

            case TRACE::CHECKPOINT:
                code = "CHKPT";
                break;

            case TRACE::SYSTEM:
                code = "SYSTM";
                break;

            case TRACE::LOB_DATA:
                code = "LOBDT";
                break;

            case TRACE::SLEEP:
                code = "SLEEP";
                break;
        }

        if (OLR_LOCALES == LOCALES::TIMESTAMP) {
            std::ostringstream s;
            char timestamp[30];
            epochToIso8601(clock->getTimeT() + logTimezone, timestamp, false, false);
            s << timestamp << " TRACE " << code << " " << message << '\n';
            std::cerr << s.str();
        } else {
            std::ostringstream s;
            s << "TRACE " << code << " " << message << '\n';
            std::cerr << s.str();
        }
    }

    void Ctx::printMemoryUsageHWM() const {
        info(0, "Memory HWM: " + std::to_string(getMemoryHWM()) + "MB, builder HWM: " +
                std::to_string(memoryModulesHWM[Ctx::MEMORY::BUILDER] * MEMORY_CHUNK_SIZE_MB) + "MB, parser HWM: " +
                std::to_string(memoryModulesHWM[Ctx::MEMORY::PARSER] * MEMORY_CHUNK_SIZE_MB) + "MB, disk read buffer HWM: " +
                std::to_string(memoryModulesHWM[Ctx::MEMORY::READER] * MEMORY_CHUNK_SIZE_MB) + "MB, transaction HWM: " +
                std::to_string(memoryModulesHWM[Ctx::MEMORY::TRANSACTIONS] * MEMORY_CHUNK_SIZE_MB) + "MB, swapped: " + std::to_string(swappedMB) + "MB");
    }

    void Ctx::printMemoryUsageCurrent() const {
        info(0, "Memory current swap: " + std::to_string(memoryChunksSwap * MEMORY_CHUNK_SIZE_MB) + "MB, allocated: " +
                std::to_string(memoryChunksAllocated * MEMORY_CHUNK_SIZE_MB) + "MB, free: " +
                std::to_string(memoryChunksFree * MEMORY_CHUNK_SIZE_MB) + "MB, memory builder: " +
                std::to_string(memoryModulesAllocated[Ctx::MEMORY::BUILDER] * MEMORY_CHUNK_SIZE_MB) + "MB, parser: " +
                std::to_string(memoryModulesAllocated[Ctx::MEMORY::PARSER] * MEMORY_CHUNK_SIZE_MB) + "MB, disk read buffer: " +
                std::to_string(memoryModulesAllocated[Ctx::MEMORY::READER] * MEMORY_CHUNK_SIZE_MB) + "MB, transaction: " +
                std::to_string(memoryModulesAllocated[Ctx::MEMORY::TRANSACTIONS] * MEMORY_CHUNK_SIZE_MB) + "MB, swapped: " + std::to_string(swappedMB) + "MB");
    }
}
