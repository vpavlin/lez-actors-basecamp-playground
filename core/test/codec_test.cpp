// Unit tests for the pure helpers (ids, balances, actor states, messages, fees).
// g++ -std=c++20 -I../src -I<nlohmann include> codec_test.cpp -o codec_test && ./codec_test
#include "lez_codec.hpp"
#include <cstdio>
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
    std::printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
