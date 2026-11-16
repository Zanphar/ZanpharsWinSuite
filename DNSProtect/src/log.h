// ============================================================================
//  log.h — lightweight, thread-safe application log.
// ============================================================================

#pragma once
#include <filesystem>
#include <string>

namespace dzp::log {

// Directs all logging to `file`; creates the parent directory if needed.
void Init(const std::filesystem::path& file);

void Info(const std::wstring& message);
void Warn(const std::wstring& message);

}  // namespace dzp::log
