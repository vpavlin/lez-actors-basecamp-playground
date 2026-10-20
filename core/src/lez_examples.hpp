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

// One message of a step, between actors written as "<account>/<program>": accounts A and B
// (example accounts 1 and 2) or payer; programs prog (the example itself), native (the native
// token) or hello (hello_world). "tx" is the transaction delivering the first message.
struct FlowMsg {
    std::string from, to;
    std::string kind;   // "root", "sync" or "async"
    std::string name;
};
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
           "In v0.4 a reverted call is still included in a block and the fee is still charged, and it "
           "costs the whole gas limit the transaction declared (here ~16 M raw units against ~60 k for "
           "the signed call below); only the state stays unchanged.",
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
        {"actor_conversation", "actor_conversation: actors talking to each other (sync, async, rollback)",
         "Written for this playground (core/examples/src/actor_conversation.rs). A program that "
         "talks to other actors and records each hop as a line in its own actor state, so the result "
         "shows the conversation, read back from the chain: who sent what to whom. A synchronous message runs right away, inside the "
         "sender's handling, but has no return value, so data comes back as a reply message that runs "
         "the sender again. An asynchronous message to a public actor runs after that, still in the "
         "same transaction, and carries no authorization. A panic in any hop undoes all of them.",
         {},
         {{"ask", "Ask the native token for the payer's balance (sync)", "applied",
           "AskBalance: actor_conversation on account 1 notes a pending request and sends ReadState "
           "to the native token's actor on the payer. The native token answers with a StateReply "
           "sent back to (account 1, actor_conversation), in this same transaction, and "
           "actor_conversation runs a second time to record the balance. Three hops, one transaction.",
           false, ""},
          {"require-ok", "Require the payer to hold at least 1000 (sync, passes)", "applied",
           "The same round trip, but the reply handler checks the balance it got back against a "
           "minimum. The payer has far more, so it records the balance.",
           false, ""},
          {"require-fail", "Require far more than the payer holds (sync, rolls back)", "reverted",
           "The reply handler panics because the balance is below the minimum. The panic is in the "
           "third hop, but the whole transaction is undone: the pending note the first hop wrote "
           "never lands, and account 1's state is exactly as before. Still included and charged.",
           false, ""},
          {"ping", "Ping example account 2, which the transaction declares (async)", "applied",
           "Ping: actor_conversation on account 1 sends Pong to actor_conversation on account 2 as an "
           "ASYNCHRONOUS message, and the transaction mentions account 2. A declared public destination "
           "runs in this transaction, so the engine promotes the message: Pong runs in this same "
           "transaction, after the synchronous work, and carries no authorization (is_authorized is "
           "false for it). Both lines land in one transaction. Only private accounts and other zones "
           "get async messages later.",
           true, "hello from account 1"},
          {"ping-undeclared", "Ping account 2 without declaring it (async, refused)", "reverted",
           "The same Ping, but the transaction doesn't mention account 2. For a public destination the "
           "engine can't promote it into this transaction, and the global mailbox (where async messages "
           "wait for a later transaction) only takes messages for private accounts. So the engine refuses "
           "the whole transaction (UnboundAsyncDestination): every async message to a public actor must be "
           "declared. An engine error, not a program panic; still included and charged.",
           true, "undeclared"}}},
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

// The messages a step sends, in delivery order, for the app's actor map. failsAt is the message
// (1-based) that panics, or that the engine refuses, in a step expected to revert.
struct StepFlow {
    std::vector<FlowMsg> messages;
    int failsAt = 0;
    bool refused = false;
};
inline StepFlow flowFor(const std::string& ex, const std::string& step) {
    if (ex == "hello_world") return {{{"tx", "A/prog", "root", "Write the text"}}};
    if (ex == "hello_world_with_authorization") {
        if (step == "unsigned") return {{{"tx", "A/prog", "root", "the text (account 1 doesn't sign)"}}, 1};
        return {{{"tx", "A/prog", "root", "the text (account 1 signs)"}}};
    }
    if (ex == "hello_world_with_move_function") {
        if (step == "write") return {{{"tx", "A/prog", "root", "Write"}}};
        if (step == "append-direct") return {{{"tx", "B/prog", "root", "Append (not from the program)"}}, 1};
        StepFlow f{{{"tx", "A/prog", "root", "MoveData { data, to: account 2 }"}, {"A/prog", "B/prog", "sync", "Append(data)"}}};
        if (step == "move-wrong") f.failsAt = 1;   // the source refuses before sending
        return f;
    }
    if (ex == "simple_tail_call")
        return {{{"tx", "A/prog", "root", "callee = (account 1, hello_world)"}, {"A/prog", "A/hello", "sync", "\"Hello from tail call\""}}};
    if (ex == "actor_conversation") {
        if (step == "ping" || step == "ping-undeclared") {
            StepFlow f{{{"tx", "A/prog", "root", "Ping { to: account 2 }"}, {"A/prog", "B/prog", "async", "Pong"}}};
            if (step == "ping-undeclared") { f.failsAt = 2; f.refused = true; }
            return f;
        }
        std::string first = step == "ask" ? "AskBalance { of: payer }" : step == "require-ok" ? "Require { of: payer, min: 1000 }" : "Require { of: payer, min: 2^100 }";
        StepFlow f{{{"tx", "A/prog", "root", first}, {"A/prog", "payer/native", "sync", "ReadState"}, {"payer/native", "A/prog", "sync", "StateReply { balance }"}}};
        if (step == "require-fail") f.failsAt = 3;   // the reply handler's check panics
        return f;
    }
    return {};
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
// actor_conversation::Message: 0 AskBalance{of}, 1 Require{of, min: u128}, 2 Ping{to, note}, 3 Pong{note}
inline Bytes acvAskBalance(const Id& of) { Bytes b{0}; b.insert(b.end(), of.begin(), of.end()); return b; }
inline Bytes acvRequire(const Id& of, u128 min) {
    Bytes b{1}; b.insert(b.end(), of.begin(), of.end());
    for (int i = 0; i < 16; i++) b.push_back(uint8_t(min >> (8 * i)));
    return b;
}
inline Bytes acvPing(const Id& to, const Bytes& note) { Bytes b{2}; b.insert(b.end(), to.begin(), to.end()); appendVec(b, note); return b; }
// an Actor is {account_id: [u8; 32], program_account_id: [u8; 32]}
inline Bytes actorBytes(const Id& account, const Id& program) {
    Bytes b(account.begin(), account.end());
    b.insert(b.end(), program.begin(), program.end());
    return b;
}

}  // namespace lez
