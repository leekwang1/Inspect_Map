#pragma once

#include "conversion_core.h"

class InputAdapters {
public:
    static bool scan(const QString &root, const QString &requestedAdapter,
                     ScanResult *result, QString *error);
};
