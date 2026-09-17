// ---------------------------------------------------------------------------
// IpcProtocol.h - newline-delimited JSON messages between the CEP panel and
// HdrHint. Every message has "kind". See docs/ARCHITECTURE.md for the table.
// ---------------------------------------------------------------------------
#pragma once

#include "core/JobModel.h"
#include "platform/Win.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace hh::ipc {

using json = nlohmann::json;

/// Parses one line; std::nullopt when it is not a JSON object with a "kind".
std::optional<json> parseLine(std::string_view line);

/// "kind" of a parsed message (empty when missing).
std::string kindOf(const json& msg);
/// Safe string field access (empty when missing / not a string).
std::string str(const json& msg, const char* key);
std::wstring wstr(const json& msg, const char* key);
double num(const json& msg, const char* key, double fallback = 0.0);
bool boolean(const json& msg, const char* key, bool fallback = false);

/// Serialises with a trailing newline.
std::string line(const json& msg);

// ---- outbound builders -------------------------------------------------------
json makeWelcome(const std::wstring& appVersion, bool docked, int protocol = 1);
json makePong();
json makeAck(const json& request, bool ok, const std::string& error = {});
json makeRequestBounds();
json makeStatus(bool docked, int encoding, int ready, int muxing, int done, int failed,
                const std::wstring& lastJobName, const std::wstring& lastJobState,
                const std::wstring& mkvmergeVersion, bool mkvmergeOk);
json makeJobsEvent(const std::vector<Job>& jobs);
json jobToJson(const Job& job);

} // namespace hh::ipc
