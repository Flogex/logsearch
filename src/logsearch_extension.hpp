#pragma once

#include <duckdb/main/extension.hpp>
#include <duckdb/main/extension/extension_loader.hpp>
#include <string>

namespace duckdb {

class LogsearchExtension final : public Extension {
public:
    void Load(ExtensionLoader& loader) override;
    [[nodiscard]] std::string Name() override;
    [[nodiscard]] std::string Version() const override;
};

} // namespace duckdb
