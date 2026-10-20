// Unit tests for the pure helpers (ids, balances, actor states, messages, fees).
// g++ -std=c++20 -I../src -I<nlohmann include> codec_test.cpp -o codec_test && ./codec_test
#include "lez_codec.hpp"
#include "lez_examples.hpp"
#include <fstream>
#include <iterator>
#include <cstdio>
namespace lez {
std::pair<const uint8_t*, size_t> exampleElf(const char*) { return {nullptr, 0}; }
std::pair<const char*, size_t> exampleSource(const char*) { return {nullptr, 0}; }
}
static int fails = 0, passes = 0;
#define CHECK(c, m) do { if (c) passes++; else { fails++; std::printf("FAIL: %s\n", m); } } while (0)
using namespace lez;
int main() {
    CHECK(b58(nativeTokenId()) == std::string(32, '1'), "native token id is 32 ones in base58");
    CHECK(b58(programLoaderId()) == "JAQ5MVHbCkSYRzXunsrNuM2m1LS859PGveHfoYPAmcvZ", "program loader id (prog-tui's vector)");
    auto pl = parseId("JAQ5MVHbCkSYRzXunsrNuM2m1LS859PGveHfoYPAmcvZ");
    CHECK(pl && *pl == programLoaderId(), "base58 round trip");
    auto pl2 = parseId("Public/JAQ5MVHbCkSYRzXunsrNuM2m1LS859PGveHfoYPAmcvZ");
    CHECK(pl2 && *pl2 == programLoaderId(), "Public/ prefix accepted");
    auto ph = parseId(std::string(64, 'f').replace(0, 2, "fe"));
    CHECK(ph && (*ph)[0] == 0xfe && (*ph)[31] == 0xff, "64 hex chars accepted");
    CHECK(!parseId("not-an-id") && !parseId("abc"), "garbage rejected");
    json acc = json::parse(R"({"nonce":9,"data":{"actor_states":{"11111111111111111111111111111111":[208,252,160,156,48,18,0,0,0,0,0,0,0,0,0,0],"7WWX":[72,111,108,97]}}})");
    auto v = parseAccount(acc);
    CHECK(v && v->nonce == 9, "nonce parsed");
    CHECK(v && u128str(v->balance()) == "19999995526352", "balance is the native actor state, LE u128 (prog-tui's vector)");
    CHECK(v && printable(v->actorStates["7WWX"]) == std::optional<std::string>("Hola"), "printable state as text");
    CHECK(!printable(Bytes{0, 1, 255}), "binary state is not text");
    CHECK(!parseAccount(json(nullptr)), "null = not on chain");
    CHECK(feeReserveEstimate(2'000'000, 8, 8) == u128(16'004'800), "fee reserve (prog-tui's vector)");
    CHECK(feeReserveEstimate(0, 8, 8) == u128(4'800), "fee reserve with no gas");
    CHECK(parseU128("340282366920938463463374607431768211455") && u128str(*parseU128("340282366920938463463374607431768211455")) == "340282366920938463463374607431768211455", "u128 max round trip");
    CHECK(!parseU128("340282366920938463463374607431768211456") && !parseU128("1.5") && !parseU128("-1") && !parseU128("1,000"), "bad amounts rejected (no silent trimming)");
    Bytes hb = borshBytes("Hola");
    CHECK(hex(hb) == "04000000486f6c61", "borsh Vec<u8>: u32 LE length + bytes");
    Id to{}; to[0] = 1;
    Bytes tm = nativeTransferMessage(0, to, u128(1000));
    CHECK(tm.size() == 1 + 32 + 16 && tm[0] == 0 && tm[1] == 1 && tm[33] == 0xe8 && tm[34] == 0x03, "Transfer{to, amount}: variant 0, id, LE u128");
    CHECK(transferOutcome(100, 50, 0, 40, 40) == TransferOutcome::Moved, "moved");
    CHECK(transferOutcome(100, 90, 0, 0, 40) == TransferOutcome::Reverted, "reverted: fee charged, nothing credited");
    CHECK(transferOutcome(100, 100, 0, 0, 40) == TransferOutcome::Unchanged, "unchanged");
    auto hx = unhex("0x0001ff");
    CHECK(hx && hex(*hx) == "0001ff", "hex round trip, 0x prefix");
    // the example programs' borsh messages
    Bytes ab{'a', 'b'};
    CHECK(hex(moveFnWrite(ab)) == "00020000006162", "move_function Write(Vec): variant 0 + vec");
    Id dst{}; dst[0] = 7; dst[31] = 9;
    Bytes md = moveFnMoveData(ab, dst);
    CHECK(md.size() == 1 + 4 + 2 + 32 && md[0] == 1 && md[7] == 7 && md[38] == 9, "move_function MoveData{data, to}: variant 1 + vec + 32-byte id");
    CHECK(hex(moveFnAppend(ab)).substr(0, 2) == "02", "move_function Append: variant 2");
    Id acct{}; acct[0] = 0xaa; Id prg{}; prg[0] = 0xbb;
    Bytes act = actorBytes(acct, prg);
    CHECK(act.size() == 64 && act[0] == 0xaa && act[32] == 0xbb, "Actor: account id then program id, 64 bytes");
    CHECK(examples().size() == 5 && findExample("simple_tail_call") && findExample("simple_tail_call")->needs.size() == 1, "example catalog");
    Bytes ask = acvAskBalance(dst);
    CHECK(ask.size() == 33 && ask[0] == 0 && ask[1] == 7, "actor_conversation AskBalance{of}: variant 0 + 32-byte id");
    Bytes req = acvRequire(dst, u128(1000));
    CHECK(req.size() == 1 + 32 + 16 && req[0] == 1 && req[33] == 0xe8 && req[34] == 0x03, "actor_conversation Require{of, min}: variant 1 + id + LE u128");
    Bytes png = acvPing(dst, ab);
    CHECK(png.size() == 1 + 32 + 4 + 2 && png[0] == 2 && png[33] == 2, "actor_conversation Ping{to, note}: variant 2 + id + vec");
    CHECK(findStep(*findExample("actor_conversation"), "ping-undeclared")->expect == "reverted", "an undeclared async destination is refused");
    for (const auto& e : examples()) for (const auto& st : e.steps) CHECK(st.expect == "applied" || st.expect == "reverted", "every step states its expected outcome");
    // the actor map's message flows
    for (const auto& e : examples()) for (const auto& st : e.steps) {
        StepFlow f = flowFor(e.name, st.id);
        CHECK(!f.messages.empty() && f.messages[0].from == "tx" && f.messages[0].kind == "root", "every step's flow starts with the transaction's root message");
        CHECK((st.expect == "reverted") == (f.failsAt > 0), "a step expected to revert names the message that fails");
    }
    CHECK(flowFor("actor_conversation", "ask").messages.size() == 3 && flowFor("actor_conversation", "ask").messages[2].to == "A/prog", "ask: request, ReadState, and the reply back to the asker");
    CHECK(flowFor("actor_conversation", "ping-undeclared").refused, "the undeclared ping is an engine refusal");
    // every message's code anchors exist in its source file
    for (const auto& e : examples()) for (const auto& st : e.steps) {
        StepFlow f = flowFor(e.name, st.id);
        for (size_t n = 1; n <= f.messages.size(); n++) {
            CodeRef c = codeFor(e.name, st.id, int(n));
            std::ifstream in(std::string(EXAMPLES_SRC) + "/" + c.file + ".rs");
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto r = resolveRanges(text, c);
            CHECK(!c.file.empty() && !text.empty() && r.size() == c.ranges.size() && !c.says.empty(),
                  (e.name + "/" + st.id + " message " + std::to_string(n) + ": code anchors found").c_str());
            if (!c.panicAnchor.empty()) CHECK(resolveRanges(text, CodeRef{c.file, {{c.panicAnchor, 1}}, ""}).size() == 1, (e.name + "/" + st.id + ": panic line found").c_str());
            if (int(n) == f.failsAt && !f.refused) CHECK(!c.panicAnchor.empty(), (e.name + "/" + st.id + ": the panicking message names its panic line").c_str());
        }
    }
    std::printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
