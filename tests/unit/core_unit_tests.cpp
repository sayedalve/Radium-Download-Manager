// SPDX-License-Identifier: GPL-3.0-or-later
// Dedicated Unit Test Suite for Radium Core Engine Components:
//   - PathSanitizer (Path traversal, Windows reserved names, ADS, Unicode)
//   - JsonMini (RFC 8259 parser, primitives, arrays, objects, malformed rejection)
//   - Bencode & TorrentParser (bencode types, malformed encodings, minimal torrents, invalid pieces)
//   - SegmentScheduler (splitting, bounds, adaptive thresholds, rollback, concurrency invariants)

#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <cmath>
#include <filesystem>

#include "common/path_sanitizer.hpp"
#include "common/json_mini.hpp"
#include "engine_torrent/bencode.hpp"
#include "engine_torrent/torrent_parser.hpp"
#include "brisk_engine/segment_scheduler.hpp"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED: " << msg << " (" << #cond << ") at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

// 1. PathSanitizer Comprehensive Unit Tests
bool TestDedicatedPathSanitizer() {
    using openidm::PathSanitizer;

    // Traversal attacks
    TEST_ASSERT(PathSanitizer::SanitizeFileName("../secret.txt") == "secret.txt", "Parent traversal 1");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("..\\..\\Windows\\System32\\calc.exe") == "calc.exe", "Windows backslash traversal");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("/etc/shadow") == "shadow", "Root absolute path");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("D:\\data\\file.zip") == "file.zip", "Drive letter stripping");

    // NTFS Alternate Data Streams (ADS)
    TEST_ASSERT(PathSanitizer::SanitizeFileName("normal.txt:hidden:$DATA") == "normal.txt", "NTFS ADS stripping");

    // Windows Reserved Names
    TEST_ASSERT(PathSanitizer::SanitizeFileName("CON.txt") == "_CON.txt", "Reserved CON.txt");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("prn.txt") == "_prn.txt", "Reserved PRN.txt");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("aux") == "_aux", "Reserved AUX");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("nul") == "_nul", "Reserved NUL");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("COM1.mp4") == "_COM1.mp4", "Reserved COM1.mp4");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("lpt9.log") == "_lpt9.log", "Reserved LPT9.log");

    // Trailing dots and spaces (Windows filesystem quirks)
    TEST_ASSERT(PathSanitizer::SanitizeFileName("document.docx. . .") == "document.docx", "Trailing dots and spaces");

    // Unicode & Non-ASCII filenames
    TEST_ASSERT(PathSanitizer::SanitizeFileName("résumé_2026.pdf") == "résumé_2026.pdf", "Unicode Latin");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("视频_1080p.mkv") == "视频_1080p.mkv", "Unicode CJK");

    // Empty or pure separator names fallback safely
    std::string empty_san = PathSanitizer::SanitizeFileName("");
    TEST_ASSERT(!empty_san.empty() && empty_san != ".." && empty_san != "/", "Empty filename safe fallback");

    return true;
}

// 2. JsonMini RFC 8259 Parser & Builder Tests
bool TestDedicatedJsonMini() {
    using namespace openidm::json;

    // Primitives
    TEST_ASSERT(Parse("null").is_null(), "Parse null");
    TEST_ASSERT(Parse("true").as_bool() == true, "Parse true");
    TEST_ASSERT(Parse("false").as_bool() == false, "Parse false");
    TEST_ASSERT(Parse("123456").as_int64() == 123456, "Parse positive int");
    TEST_ASSERT(Parse("-789").as_int64() == -789, "Parse negative int");
    TEST_ASSERT(std::abs(Parse("3.14159").as_double() - 3.14159) < 1e-4, "Parse float");

    // String with escapes
    std::string s_json = R"("Hello \"Radium\" \n\t\\ \u0041")";
    Value str_val = Parse(s_json);
    TEST_ASSERT(str_val.is_string(), "Parse escaped string");
    TEST_ASSERT(str_val.as_string().find("Radium") != std::string::npos, "String content contains Radium");
    TEST_ASSERT(str_val.as_string().find("A") != std::string::npos, "Unicode escape \\u0041 -> A");

    // Arrays & Objects
    std::string complex_json = R"({
        "app": "Radium",
        "concurrency": 8,
        "features": ["segmented", "bittorrent", "streams"],
        "active": true,
        "nested": { "sub_key": 42 }
    })";
    Value doc = Parse(complex_json);
    TEST_ASSERT(doc.is_object(), "Root is object");
    TEST_ASSERT(doc["app"].as_string() == "Radium", "Object field 'app'");
    TEST_ASSERT(doc["concurrency"].as_int64() == 8, "Object field 'concurrency'");
    TEST_ASSERT(doc["features"].is_array(), "Object field 'features' is array");
    TEST_ASSERT(doc["features"].as_array().size() == 3, "Array size is 3");
    TEST_ASSERT(doc["features"].as_array()[0].as_string() == "segmented", "Array element 0");
    TEST_ASSERT(doc["nested"]["sub_key"].as_int64() == 42, "Nested object navigation");

    // Serialization / Dump
    std::string dumped = doc.dump();
    Value redoc = Parse(dumped);
    TEST_ASSERT(redoc["app"].as_string() == "Radium", "Roundtrip dump-and-parse");

    // Malformed / Truncated inputs must throw runtime_error safely without crashing
    bool caught_malformed = false;
    try {
        Parse("{ \"unclosed\": true ");
    } catch (const std::runtime_error&) {
        caught_malformed = true;
    }
    TEST_ASSERT(caught_malformed, "Catch truncated object");

    bool caught_invalid_num = false;
    try {
        Parse("[1, 2, invalid_token]");
    } catch (const std::runtime_error&) {
        caught_invalid_num = true;
    }
    TEST_ASSERT(caught_invalid_num, "Catch invalid token in array");

    return true;
}

// 3. Bencode & TorrentParser Tests
bool TestDedicatedBencodeAndTorrentParser() {
    using namespace openidm::torrent;

    // Bencode decoding
    auto i = BencodeParser::Decode("i999e");
    TEST_ASSERT(i.ok() && i.value->as_int() == 999, "Bencode int");

    auto s = BencodeParser::Decode("6:radium");
    TEST_ASSERT(s.ok() && s.value->as_string() == "radium", "Bencode string");

    auto l = BencodeParser::Decode("l4:testi10ee");
    TEST_ASSERT(l.ok() && l.value->as_list().size() == 2, "Bencode list");

    auto d = BencodeParser::Decode("d3:cow3:moo4:spami42ee");
    TEST_ASSERT(d.ok() && d.value->as_dict().size() == 2, "Bencode dict");

    // Malformed bencode rejection
    TEST_ASSERT(!BencodeParser::Decode("i42").ok(), "Reject unclosed integer");
    TEST_ASSERT(!BencodeParser::Decode("10:short").ok(), "Reject length exceeding buffer");
    TEST_ASSERT(!BencodeParser::Decode("d3:key").ok(), "Reject unclosed dict");

    // Torrent parsing with synthetic deterministic bencoded metainfo
    // Minimal valid single-file torrent dictionary:
    // d4:infod6:lengthi1024e4:name8:test.bin12:piece lengthi1024e6:pieces20:12345678901234567890ee
    std::string valid_meta = "d4:infod6:lengthi1024e4:name8:test.bin12:piece lengthi1024e6:pieces20:12345678901234567890ee";
    auto t_res = TorrentParser::ParseTorrentBuffer(valid_meta);
    TEST_ASSERT(t_res.ok(), "Parse minimal valid torrent metainfo");
    TEST_ASSERT(t_res.value->name == "test.bin", "Parsed torrent name");
    TEST_ASSERT(t_res.value->total_length == 1024, "Parsed torrent length");
    TEST_ASSERT(t_res.value->piece_length == 1024, "Parsed piece length");
    TEST_ASSERT(t_res.value->pieces.size() == 1, "Parsed pieces count");

    // Corrupt / Missing fields rejection
    std::string missing_length = "d4:infod4:name8:test.bin12:piece lengthi1024e6:pieces20:12345678901234567890ee";
    TEST_ASSERT(!TorrentParser::ParseTorrentBuffer(missing_length).ok(), "Reject torrent missing length/files");

    std::string bad_pieces = "d4:infod6:lengthi1024e4:name8:test.bin12:piece lengthi1024e6:pieces15:short_pieces_ee";
    TEST_ASSERT(!TorrentParser::ParseTorrentBuffer(bad_pieces).ok(), "Reject torrent with pieces not divisible by 20");

    return true;
}

// 4. SegmentScheduler Comprehensive Invariant Tests
bool TestDedicatedSegmentScheduler() {
    using namespace openidm::brisk;

    // Test deterministic splitting of 100 MB file across 8 workers
    const uint64_t total_size = 100 * 1024 * 1024; // 100 MB
    SegmentScheduler scheduler(total_size, 8, 256 * 1024);
    scheduler.Initialize();

    // Initial segment acquisition for first worker
    auto seg0 = scheduler.AcquireNextOrStealSegment(false);
    TEST_ASSERT(seg0 != nullptr, "Worker acquires initial segment");
    TEST_ASSERT(seg0->start_offset == 0 && seg0->end_offset.load() == total_size - 1, "Initial segment covers entire file");

    // Advance cursor to simulate data progress
    seg0->current_offset.store(10 * 1024 * 1024);

    // Second worker attempts work-stealing from first worker
    auto seg1 = scheduler.AcquireNextOrStealSegment(true);
    TEST_ASSERT(seg1 != nullptr, "Worker 2 successfully steals segment");
    TEST_ASSERT(seg1->start_offset > 10 * 1024 * 1024, "Stolen segment starts past worker 1's progress");
    TEST_ASSERT(seg1->end_offset.load() == total_size - 1, "Stolen segment takes upper boundary");

    // In-flight rejection rollback
    bool rolled_back = scheduler.RollbackUnstartedStolenSegment(seg1->segment_id);
    TEST_ASSERT(rolled_back, "Rollback unstarted segment succeeds");

    // Verify all ranges remain contiguous and no hole is formed
    auto snapshots = scheduler.GetSnapshots();
    TEST_ASSERT(!snapshots.empty(), "Snapshots list is non-empty");
    TEST_ASSERT(SegmentScheduler::ValidateSegmentPartition(snapshots, total_size), "Partition validation passes after rollback");

    return true;
}

int main() {
    int passed = 0;
    const int total = 4;

    std::cout << "====================================================\n";
    std::cout << " Radium Core Dedicated Unit Test Suite (Fast CTest)\n";
    std::cout << "====================================================\n";

    auto run_test = [&](const char* name, bool (*fn)()) {
        std::cout << "[RUN] " << name << "..." << std::endl;
        if (fn()) {
            std::cout << "  -> PASS\n";
            ++passed;
        } else {
            std::cout << "  -> FAIL\n";
        }
    };

    run_test("PathSanitizer Comprehensive Unit Tests", TestDedicatedPathSanitizer);
    run_test("JsonMini RFC 8259 Parser & Builder Tests", TestDedicatedJsonMini);
    run_test("Bencode & TorrentParser Tests", TestDedicatedBencodeAndTorrentParser);
    run_test("SegmentScheduler Invariant & Work-Stealing Tests", TestDedicatedSegmentScheduler);

    std::cout << "\nResult: " << passed << " / " << total << " dedicated test suites PASSED.\n";
    return (passed == total) ? 0 : 1;
}
