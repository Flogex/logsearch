#pragma once

#include "duckdb/main/extension.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <string>

namespace duckdb {

class LogsearchExtension final : public Extension {
public:
    static void LoadInternal(ExtensionLoader& loader);

    void Load(ExtensionLoader& loader) override;
    std::string Name() override;
    std::string Version() const override;
};

} // namespace duckdb
