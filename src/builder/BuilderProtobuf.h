/* Header for BuilderProtobuf class
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

#include "../common/DbTable.h"
#include "../common/OraProtoBuf.pb.h"
#include "../metadata/Metadata.h"
#include "../metadata/Schema.h"
#include "Builder.h"

#ifndef BUILDER_PROTOBUF_H_
#define BUILDER_PROTOBUF_H_

namespace OpenLogReplicator {
    class BuilderProtobuf final : public Builder {
    protected:
        pb::RedoResponse* redoResponsePB;
        pb::Value* valuePB;
        pb::Payload* payloadPB;
        pb::Schema* schemaPB;

        inline void columnNull(const DbTable* table, typeCol col, bool after) {
            if (table != nullptr && unknownType == UNKNOWN_TYPE::UNKNOWN_HIDE) {
                DbColumn* column = table->columns[col];
                if (column->storedAsLob)
                    return;
                if (column->guard && !ctx->isFlagSet(Ctx::REDO_FLAGS::SHOW_GUARD_COLUMNS))
                    return;
                if (column->nested && !ctx->isFlagSet(Ctx::REDO_FLAGS::SHOW_NESTED_COLUMNS))
                    return;
                if (column->hidden && !ctx->isFlagSet(Ctx::REDO_FLAGS::SHOW_HIDDEN_COLUMNS))
                    return;
                if (column->unused && !ctx->isFlagSet(Ctx::REDO_FLAGS::SHOW_UNUSED_COLUMNS))
                    return;

                typeType typeNo = table->columns[col]->type;
                if (typeNo != SysCol::COLTYPE::VARCHAR
                    && typeNo != SysCol::COLTYPE::NUMBER
                    && typeNo != SysCol::COLTYPE::DATE
                    && typeNo != SysCol::COLTYPE::RAW
                    && typeNo != SysCol::COLTYPE::CHAR
                    && typeNo != SysCol::COLTYPE::FLOAT
                    && typeNo != SysCol::COLTYPE::DOUBLE
                    && (typeNo != SysCol::COLTYPE::XMLTYPE || !after)
                    && (typeNo != SysCol::COLTYPE::JSON || !after)
                    && (typeNo != SysCol::COLTYPE::CLOB || !after)
                    && (typeNo != SysCol::COLTYPE::BLOB || !after)
                    && typeNo != SysCol::COLTYPE::TIMESTAMP
                    && typeNo != SysCol::COLTYPE::INTERVAL_YEAR_TO_MONTH
                    && typeNo != SysCol::COLTYPE::INTERVAL_DAY_TO_SECOND
                    && typeNo != SysCol::COLTYPE::UROWID
                    && typeNo != SysCol::COLTYPE::TIMESTAMP_WITH_LOCAL_TZ)
                    return;
            }

            if (table == nullptr || ctx->isFlagSet(Ctx::REDO_FLAGS::RAW_COLUMN_DATA)) {
                std::string columnName("COL_" + std::to_string(col));
                valuePB->set_name(columnName);
                return;
            }

            valuePB->set_name(table->columns[col]->name);
        }

        inline void appendRowid(typeDataObj dataObj, typeDba bdba, typeSlot slot) {
            if ((messageFormat & MESSAGE_FORMAT::MSG_ADD_SEQUENCES) != 0)
                payloadPB->set_num(num);

            if (ridFormat == RID_FORMAT::RID_SKIP)
                return;
            else if (ridFormat == RID_FORMAT::RID_TEXT) {
                typeRowId rowId(dataObj, bdba, slot);
                char str[19];
                rowId.toString(str);
                payloadPB->set_rid(str, 18);
            }
        }

        inline void appendHeader(typeScn scn, time_t timestamp, bool first, bool showDb, bool showXid) {
            redoResponsePB->set_code(pb::ResponseCode::PAYLOAD);
            if (first || (scnType & SCN_TYPE::SCN_ALL_PAYLOADS) != 0) {
                if (scnFormat == SCN_FORMAT::SCN_TEXT_HEX) {
                    char buf[17];
                    numToString(scn, buf, 16);
                    redoResponsePB->set_scns(buf);
                } else {
                    redoResponsePB->set_scn(scn);
                }
            }

            if (first || (timestampAll & TIMESTAMP_ALL::TIMESTAMP_ALL_PAYLOADS) != 0) {
                std::string str;
                switch (timestampFormat) {
                    case TIMESTAMP_FORMAT::TMSTP_UNIX_NANO:
                        redoResponsePB->set_tm(timestamp * 1000000000L);
                        break;

                    case TIMESTAMP_FORMAT::TMSTP_UNIX_MICRO:
                        redoResponsePB->set_tm(timestamp * 1000000L);
                        break;

                    case TIMESTAMP_FORMAT::TMSTP_UNIX_MILLI:
                        redoResponsePB->set_tm(timestamp * 1000L);
                        break;

                    case TIMESTAMP_FORMAT::TMSTP_UNIX:
                        redoResponsePB->set_tm(timestamp);
                        break;

                    case TIMESTAMP_FORMAT::TMSTP_UNIX_NANO_STRING:
                        str = std::to_string(timestamp * 1000000000L);
                        redoResponsePB->set_tms(str);
                        break;

                    case TIMESTAMP_FORMAT::TMSTP_UNIX_MICRO_STRING:
                        str = std::to_string(timestamp * 1000000L);
                        redoResponsePB->set_tms(str);
                        break;

                    case TIMESTAMP_FORMAT::TMSTP_UNIX_MILLI_STRING:
                        str = std::to_string(timestamp * 1000L);
                        redoResponsePB->set_tms(str);
                        break;

                    case TIMESTAMP_FORMAT::TMSTP_UNIX_STRING:
                        str = std::to_string(timestamp);
                        redoResponsePB->set_tms(str);
                        break;

                    case TIMESTAMP_FORMAT::TMSTP_ISO8601:
                        char buffer[22];
                        str.assign(buffer, ctx->epochToIso8601(timestamp, buffer, true, true));
                        redoResponsePB->set_tms(str);
                        break;

                    default:
                        break;
                }
            }

            redoResponsePB->set_c_scn(lwnScn);
            redoResponsePB->set_c_idx(lwnIdx);

            if (showXid) {
                if (xidFormat == XID_FORMAT::XID_TEXT_HEX) {
                    std::ostringstream ss;
                    ss << "0x";
                    ss << std::setfill('0') << std::setw(4) << std::hex << static_cast<uint64_t>(lastXid.usn());
                    ss << '.';
                    ss << std::setfill('0') << std::setw(3) << std::hex << static_cast<uint64_t>(lastXid.slt());
                    ss << '.';
                    ss << std::setfill('0') << std::setw(8) << std::hex << static_cast<uint64_t>(lastXid.sqn());
                    redoResponsePB->set_xid(ss.str());
                } else if (xidFormat == XID_FORMAT::XID_TEXT_DEC) {
                    std::ostringstream ss;
                    ss << static_cast<uint64_t>(lastXid.usn());
                    ss << '.';
                    ss << static_cast<uint64_t>(lastXid.slt());
                    ss << '.';
                    ss << static_cast<uint64_t>(lastXid.sqn());
                    redoResponsePB->set_xid(ss.str());
                } else if (xidFormat == XID_FORMAT::XID_NUMERIC) {
                    redoResponsePB->set_xidn(lastXid.getData());
                }
            }

            if (showDb)
                redoResponsePB->set_db(metadata->conName);
        }

        inline void appendSchema(const DbTable* table, typeObj obj) {
            if (table == nullptr) {
                std::string ownerName;
                std::string tableName;
                // try to read object name from ongoing uncommitted transaction data
                if (metadata->schema->checkTableDictUncommitted(obj, ownerName, tableName)) {
                    schemaPB->set_owner(ownerName);
                    schemaPB->set_name(tableName);
                } else {
                    tableName = "OBJ_" + std::to_string(obj);
                    schemaPB->set_name(tableName);
                }

                if ((schemaFormat & SCHEMA_FORMAT::SCHEMA_OBJ) != 0)
                    schemaPB->set_obj(obj);

                return;
            }

            schemaPB->set_owner(table->owner);
            schemaPB->set_name(table->name);

            if ((schemaFormat & SCHEMA_FORMAT::SCHEMA_OBJ) != 0)
                schemaPB->set_obj(obj);

            if ((schemaFormat & SCHEMA_FORMAT::SCHEMA_FULL) != 0) {
                if ((schemaFormat & SCHEMA_FORMAT::SCHEMA_REPEATED) == 0) {
                    if (tables.count(table) > 0)
                        return;
                    else
                        tables.insert(table);
                }

                schemaPB->add_column();
                pb::Column* columnPB = schemaPB->mutable_column(schemaPB->column_size() - 1);

                for (typeCol column = 0; column < static_cast<typeCol>(table->columns.size()); ++column) {
                    if (table->columns[column] == nullptr)
                        continue;

                    columnPB->set_name(table->columns[column]->name);

                    switch (table->columns[column]->type) {
                        case SysCol::COLTYPE::VARCHAR:
                            columnPB->set_type(pb::VARCHAR2);
                            columnPB->set_length(static_cast<int32_t>(table->columns[column]->length));
                            break;

                        case SysCol::COLTYPE::NUMBER:
                            columnPB->set_type(pb::NUMBER);
                            columnPB->set_precision(static_cast<int32_t>(table->columns[column]->precision));
                            columnPB->set_scale(static_cast<int32_t>(table->columns[column]->scale));
                            break;

                        case SysCol::COLTYPE::LONG:
                            // Long, not supported
                            columnPB->set_type(pb::LONG);
                            break;

                        case SysCol::COLTYPE::DATE:
                            columnPB->set_type(pb::DATE);
                            break;

                        case SysCol::COLTYPE::RAW:
                            columnPB->set_type(pb::RAW);
                            columnPB->set_length(static_cast<int32_t>(table->columns[column]->length));
                            break;

                        case SysCol::COLTYPE::LONG_RAW: // Not supported
                            columnPB->set_type(pb::LONG_RAW);
                            break;

                        case SysCol::COLTYPE::CHAR:
                            columnPB->set_type(pb::CHAR);
                            columnPB->set_length(static_cast<int32_t>(table->columns[column]->length));
                            break;

                        case SysCol::COLTYPE::FLOAT:
                            columnPB->set_type(pb::BINARY_FLOAT);
                            break;

                        case SysCol::COLTYPE::DOUBLE:
                            columnPB->set_type(pb::BINARY_DOUBLE);
                            break;

                        case SysCol::COLTYPE::CLOB:
                            columnPB->set_type(pb::CLOB);
                            break;

                        case SysCol::COLTYPE::BLOB:
                            columnPB->set_type(pb::BLOB);
                            break;

                        case SysCol::COLTYPE::TIMESTAMP:
                            columnPB->set_type(pb::TIMESTAMP);
                            columnPB->set_length(static_cast<int32_t>(table->columns[column]->length));
                            break;

                        case SysCol::COLTYPE::TIMESTAMP_WITH_TZ:
                            columnPB->set_type(pb::TIMESTAMP_WITH_TZ);
                            columnPB->set_length(static_cast<int32_t>(table->columns[column]->length));
                            break;

                        case SysCol::COLTYPE::INTERVAL_YEAR_TO_MONTH:
                            columnPB->set_type(pb::INTERVAL_YEAR_TO_MONTH);
                            columnPB->set_length(static_cast<int32_t>(table->columns[column]->length));
                            break;

                        case SysCol::COLTYPE::INTERVAL_DAY_TO_SECOND:
                            columnPB->set_type(pb::INTERVAL_DAY_TO_SECOND);
                            columnPB->set_length(static_cast<int32_t>(table->columns[column]->length));
                            break;

                        case SysCol::COLTYPE::UROWID:
                            columnPB->set_type(pb::UROWID);
                            columnPB->set_length(static_cast<int32_t>(table->columns[column]->length));
                            break;

                        case SysCol::COLTYPE::TIMESTAMP_WITH_LOCAL_TZ:
                            columnPB->set_type(pb::TIMESTAMP_WITH_LOCAL_TZ);
                            columnPB->set_length(static_cast<int32_t>(table->columns[column]->length));
                            break;

                        default:
                            columnPB->set_type(pb::UNKNOWN);
                            break;
                    }

                    columnPB->set_nullable(table->columns[column]->nullable);
                }
            }
        }

        inline void appendAfter(LobCtx* lobCtx, const XmlCtx* xmlCtx, const DbTable* table, uint64_t offset) {
            if (columnFormat > 0 && table != nullptr) {
                for (typeCol column = 0; column < table->maxSegCol; ++column) {
                    if (values[column][VALUE_TYPE::AFTER] != nullptr) {
                        if (sizes[column][VALUE_TYPE::AFTER] > 0) {
                            payloadPB->add_after();
                            valuePB = payloadPB->mutable_after(payloadPB->after_size() - 1);
                            processValue(lobCtx, xmlCtx, table, column, values[column][VALUE_TYPE::AFTER], sizes[column][VALUE_TYPE::AFTER], offset,
                                         true, compressedAfter);
                        } else {
                            payloadPB->add_after();
                            valuePB = payloadPB->mutable_after(payloadPB->after_size() - 1);
                            columnNull(table, column, true);
                        }
                    }
                }
            } else {
                uint64_t baseMax = valuesMax >> 6;
                for (uint64_t base = 0; base <= baseMax; ++base) {
                    auto column = static_cast<typeCol>(base << 6);
                    for (uint64_t mask = 1; mask != 0; mask <<= 1, ++column) {
                        if (valuesSet[base] < mask)
                            break;
                        if ((valuesSet[base] & mask) == 0)
                            continue;

                        if (values[column][VALUE_TYPE::AFTER] != nullptr) {
                            if (sizes[column][VALUE_TYPE::AFTER] > 0) {
                                payloadPB->add_after();
                                valuePB = payloadPB->mutable_after(payloadPB->after_size() - 1);
                                processValue(lobCtx, xmlCtx, table, column, values[column][VALUE_TYPE::AFTER], sizes[column][VALUE_TYPE::AFTER], offset,
                                             true, compressedAfter);
                            } else {
                                payloadPB->add_after();
                                valuePB = payloadPB->mutable_after(payloadPB->after_size() - 1);
                                columnNull(table, column, true);
                            }
                        }
                    }
                }
            }
        }

        inline void appendBefore(LobCtx* lobCtx, const XmlCtx* xmlCtx, const DbTable* table, uint64_t offset) {
            if (columnFormat > 0 && table != nullptr) {
                for (typeCol column = 0; column < table->maxSegCol; ++column) {
                    if (values[column][VALUE_TYPE::BEFORE] != nullptr) {
                        if (sizes[column][VALUE_TYPE::BEFORE] > 0) {
                            payloadPB->add_before();
                            valuePB = payloadPB->mutable_before(payloadPB->before_size() - 1);
                            processValue(lobCtx, xmlCtx, table, column, values[column][VALUE_TYPE::BEFORE], sizes[column][VALUE_TYPE::BEFORE], offset,
                                         false, compressedBefore);
                        } else {
                            payloadPB->add_before();
                            valuePB = payloadPB->mutable_before(payloadPB->before_size() - 1);
                            columnNull(table, column, false);
                        }
                    }
                }
            } else {
                uint64_t baseMax = valuesMax >> 6;
                for (uint64_t base = 0; base <= baseMax; ++base) {
                    auto column = static_cast<typeCol>(base << 6);
                    for (uint64_t mask = 1; mask != 0; mask <<= 1, ++column) {
                        if (valuesSet[base] < mask)
                            break;
                        if ((valuesSet[base] & mask) == 0)
                            continue;

                        if (values[column][VALUE_TYPE::BEFORE] != nullptr) {
                            if (sizes[column][VALUE_TYPE::BEFORE] > 0) {
                                payloadPB->add_before();
                                valuePB = payloadPB->mutable_before(payloadPB->before_size() - 1);
                                processValue(lobCtx, xmlCtx, table, column, values[column][VALUE_TYPE::BEFORE], sizes[column][VALUE_TYPE::BEFORE], offset,
                                             false, compressedBefore);
                            } else {
                                payloadPB->add_before();
                                valuePB = payloadPB->mutable_before(payloadPB->before_size() - 1);
                                columnNull(table, column, false);
                            }
                        }
                    }
                }
            }
        }

        inline void createResponse() {
            if (unlikely(redoResponsePB != nullptr))
                throw RuntimeException(50016, "PB commit processing failed, message already exists");
            redoResponsePB = new pb::RedoResponse;
        }

        void numToString(uint64_t value, char* buf, uint64_t size) {
            uint64_t j = (size - 1) * 4;
            for (uint64_t i = 0; i < size; ++i) {
                buf[i] = Ctx::map16((value >> j) & 0xF);
                j -= 4;
            }
            buf[size] = 0;
        }

        virtual void columnFloat(const std::string& columnName, double value) override;
        virtual void columnDouble(const std::string& columnName, long double value) override;
        virtual void columnString(const std::string& columnName) override;
        virtual void columnNumber(const std::string& columnName, int precision, int scale) override;
        virtual void columnRaw(const std::string& columnName, const uint8_t* data, uint64_t size) override;
        virtual void columnRowId(const std::string& columnName, typeRowId rowId) override;
        virtual void columnTimestamp(const std::string& columnName, time_t timestamp, uint64_t fraction) override;
        virtual void columnTimestampTz(const std::string& columnName, time_t timestamp, uint64_t fraction, const char* tz) override;
        virtual void processInsert(typeScn scn, typeSeq sequence, time_t timestamp, LobCtx* lobCtx, const XmlCtx* xmlCtx, const DbTable* table, typeObj obj,
                                   typeDataObj dataObj, typeDba bdba, typeSlot slot, typeXid xid, uint64_t offset) override;
        virtual void processUpdate(typeScn scn, typeSeq sequence, time_t timestamp, LobCtx* lobCtx, const XmlCtx* xmlCtx, const DbTable* table, typeObj obj,
                                   typeDataObj dataObj, typeDba bdba, typeSlot slot, typeXid xid, uint64_t offset) override;
        virtual void processDelete(typeScn scn, typeSeq sequence, time_t timestamp, LobCtx* lobCtx, const XmlCtx* xmlCtx, const DbTable* table, typeObj obj,
                                   typeDataObj dataObj, typeDba bdba, typeSlot slot, typeXid xid, uint64_t offset) override;
        virtual void processDdl(typeScn scn, typeSeq sequence, time_t timestamp, const DbTable* table, typeObj obj, typeDataObj dataObj, uint16_t ddlType,
                                uint16_t seq, const char* sql, uint64_t sqlSize) override;
        void processBeginMessage(typeScn scn, typeSeq sequence, time_t timestamp) override;

    public:
        BuilderProtobuf(Ctx* newCtx, Locales* newLocales, Metadata* newMetadata, uint64_t newDbFormat, uint64_t newAttributesFormat,
                        INTERVAL_DTS_FORMAT newIntervalDtsFormat, INTERVAL_YTM_FORMAT newIntervalYtmFormat, uint newMessageFormat, RID_FORMAT newRidFormat,
                        XID_FORMAT newXidFormat, TIMESTAMP_FORMAT newTimestampFormat, TIMESTAMP_TZ_FORMAT newTimestampTzFormat, TIMESTAMP_ALL newTimestampAll,
                        CHAR_FORMAT newCharFormat, SCN_FORMAT newScnFormat, uint newScnType, UNKNOWN_FORMAT newUnknownFormat, uint newSchemaFormat,
                        COLUMN_FORMAT newColumnFormat, UNKNOWN_TYPE newUnknownType, uint64_t newFlushBuffer);
        virtual ~BuilderProtobuf() override;

        virtual void initialize() override;
        virtual void processCommit(typeScn scn, typeSeq sequence, time_t timestamp) override;
        virtual void processCheckpoint(typeScn scn, typeSeq sequence, time_t timestamp, uint64_t offset, bool redo) override;
    };
}

#endif
