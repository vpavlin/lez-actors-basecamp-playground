#pragma once
// The guided examples: LEZ's program-deployment tutorial programs (core/examples/*.bin), what each
// shows about v0.4, and the borsh messages their steps send. Pure, no I/O: the module runs them.
#include <string>
#include <utility>
#include <vector>
#include "lez_codec.hpp"

namespace lez {

// core/examples/*.bin, embedded at build time (embed_examples.cmake)
std::pair<const uint8_t*, size_t> exampleElf(const char* name);

struct ExampleStep {
    std::string id, label;
    std::string expect;     // "applied" or "reverted"
    std::string explain;    // what to look at, in v0.4 terms
    bool takesText = false; // the step sends text the user can edit
    std::string defaultText;
};
struct Example {
    std::string name, title, about;
    std::vector<std::string> needs;   // other examples it calls (deployed first)
    std::vector<ExampleStep> steps;
};

inline const std::vector<Example>& examples() {
    static const std::vector<Example> all = {
        {"hello_world", "hello_world: write to your actor state",
         "Appends the message to hello_world's actor state on an account. One account holds a "
         "separate state per program: the native token's balance and hello_world's bytes sit side "
         "by side on the same account.",
         {},
         {{"write", "Write a greeting", "applied",
           "The root actor is (example account 1, hello_world). Only the fee payer signs: hello_world "
           "doesn't check authorization. Watch a hello_world actor state appear on the account.",
           true, "Hola mundo!"}}},
        {"hello_world_with_authorization", "hello_world_with_authorization: is_authorized",
         "Same as hello_world, but it asserts input.is_authorized: the receiving account must sign "
         "(or a calling program must grant it with a PDA seed).",
         {},
         {{"unsigned", "Call without the account's signature", "reverted",
           "The account doesn't sign, so the guest panics with \"Missing required authorization\". "
           "In v0.4 a reverted call is still included in a block and the fee is still charged; only "
           "the state stays unchanged.",
           true, "no signature"},
          {"signed", "Call with the account's signature", "applied",
           "Now the account authorizes its mention, the guest sees is_authorized = true, and the "
           "bytes are appended.",
           true, "signed"}}},
        {"hello_world_with_move_function", "hello_world_with_move_function: moving state between accounts",
         "A program with three messages: Write appends to an account; MoveData moves bytes from one "
         "account's actor state to another account's, under the same program, by sending itself an "
         "Append; Append is refused unless it comes from the program itself.",
         {},
         {{"write", "Write to example account 1", "applied",
           "Write(data) on (account 1, this program).", true, "move me"},
          {"move", "Move account 1's bytes to account 2", "applied",
           "MoveData{data, to}: the source checks it holds exactly `data`, clears itself, and sends "
           "Append(data) to (account 2, the same program). Both actors are mentioned in the "
           "transaction. Account 1 ends empty and account 2 gains the bytes.",
           false, ""},
          {"move-wrong", "Move bytes account 1 doesn't hold", "reverted",
           "MoveData with data that differs from account 1's state: the guest panics (\"the source "
           "account does not hold the bytes...\"), and nothing moves. Still charged.",
           false, ""},
          {"append-direct", "Call Append directly on account 2", "reverted",
           "Append from outside the program: from_own_program() is false, so the guest refuses (\"only "
           "a move of this program appends...\"). A program can tell who called it.",
           true, "sneaky"}}},
        {"simple_tail_call", "simple_tail_call: one program calling another",
         "Keeps its own state unchanged and sends \"Hello from tail call\" to a callee actor named in "
         "its message. Here the callee is (the same account, hello_world), so the greeting lands in "
         "hello_world's actor state, written by a different program's call.",
         {"hello_world"},
         {{"call", "Call it, with hello_world as the callee", "applied",
           "The root actor is (account 1, simple_tail_call); the message is the callee Actor "
           "(account 1, hello_world); both actors are mentioned. The change shows up in "
           "hello_world's state, not in simple_tail_call's.",
           false, ""}}},
    };
    return all;
}

inline const Example* findExample(const std::string& name) {
    for (const auto& e : examples()) if (e.name == name) return &e;
    return nullptr;
}
inline const ExampleStep* findStep(const Example& e, const std::string& id) {
    for (const auto& s : e.steps) if (s.id == id) return &s;
    return nullptr;
}

// borsh messages of the example programs
inline void appendVec(Bytes& out, const Bytes& v) {
    uint32_t n = uint32_t(v.size());
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(n >> (8 * i)));
    out.insert(out.end(), v.begin(), v.end());
}
// hello_world_with_move_function::Message: 0 Write(Vec<u8>), 1 MoveData{data, to}, 2 Append(Vec<u8>)
inline Bytes moveFnWrite(const Bytes& data) { Bytes b{0}; appendVec(b, data); return b; }
inline Bytes moveFnMoveData(const Bytes& data, const Id& to) { Bytes b{1}; appendVec(b, data); b.insert(b.end(), to.begin(), to.end()); return b; }
inline Bytes moveFnAppend(const Bytes& data) { Bytes b{2}; appendVec(b, data); return b; }
// an Actor is {account_id: [u8; 32], program_account_id: [u8; 32]}
inline Bytes actorBytes(const Id& account, const Id& program) {
    Bytes b(account.begin(), account.end());
    b.insert(b.end(), program.begin(), program.end());
    return b;
}

}  // namespace lez
