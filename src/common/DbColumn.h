/* Header for DbColumn class
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

#include "types.h"
#include "table/SysCol.h"

#ifndef DB_COLUMN_H_
#define DB_COLUMN_H_

namespace OpenLogReplicator {
    class DbColumn final {
    public:
        typeCol col;
        typeCol guardSeg;
        typeCol segCol;
        std::string name;
        typeType type;
        uint length;
        int precision;
        int scale;
        uint64_t charsetId;
        typeCol numPk;
        bool nullable;
        bool hidden;
        bool storedAsLob;
        bool systemGenerated;
        bool nested;
        bool unused;
        bool added;
        bool guard;
        bool xmlType;
        bool nullWarning;

        DbColumn(typeCol newCol, typeCol newGuardSeg, typeCol newSegCol, const std::string& newName, typeType newType, uint newLength,
                 int newPrecision, int newScale, uint64_t newCharsetId, typeCol newNumPk, bool newNullable, bool newHidden,
                 bool newStoredAsLob, bool newSystemGenerated, bool newNested, bool newUnused, bool newAdded, bool newGuard, bool newXmlType);

        friend std::ostream& operator<<(std::ostream& os, const DbColumn& column);
    };
}

#endif