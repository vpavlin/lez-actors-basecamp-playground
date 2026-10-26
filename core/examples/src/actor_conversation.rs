// actor_conversation: actors talking to each other (LEZ Actors playground example).
//
// LEZ v0.4 programs talk by sending messages to actors, (account, program) pairs. This program
// shows the three things that makes possible, and emits an event at every hop so the app can draw
// the conversation:
//
// - `AskBalance { of }`: a SYNCHRONOUS request/reply. It sends `ReadState` to the native token's
//   actor on `of`; the native token answers with a `StateReply` sent back to this actor, in the
//   same transaction, and this program runs a second time to handle it. A synchronous message has
//   no return value: data comes back as a reply message. The request leaves a pending note in this
//   actor's state; the reply finds it (the second run sees the first run's state) and replaces it
//   with the balance it read.
// - `Require { of, min }`: the same read, but the reply handler panics when the balance is below
//   `min`. A panic in any hop undoes the whole transaction: the pending note the first hop wrote
//   is gone too, and this actor's state is exactly as before. (Still included and charged.)
// - `Ping { to, note }`: an ASYNCHRONOUS message to this program's actor on another account. To a
//   public actor that the transaction declares, the engine runs it in the same transaction, after
//   the synchronous work, with no authorization (is_authorized is false). Undeclared, it is refused
//   (UnboundAsyncDestination): only private accounts get async messages later, through a mailbox.
//
// The state is the last few hops this actor took part in, one line each ("->" sent, "<-" received),
// so the conversation can be read back from the chain: a sync round trip adds its "->" and "<-"
// lines to the same actor in one transaction; a ping's "<-" line lands on the receiver's actor.
// Each hop also emits an event (selector b"acv_hop_") carrying a structured `Hop` (the acting
// account, the peer account, the hop kind, and the sentence), kept once the block finalizes. The
// app can draw the whole actor map from these events alone once they arrive; the per-actor state
// above is still the conversation's human-readable log.
use borsh::{BorshDeserialize, BorshSerialize};
use lee_core::{
    account::{AccountId, Actor},
    native_token::{self, NATIVE_TOKEN_PROGRAM_ID, decode_balance},
    program::{ProgramEvent, ReceiveInput, Response, StateReply},
};

#[derive(BorshSerialize, BorshDeserialize)]
enum Message {
    AskBalance { of: AccountId },
    Require { of: AccountId, min: u128 },
    Ping { to: AccountId, note: Vec<u8> },
    Pong { note: Vec<u8> },
}

/// Every hop's event carries this selector. Its data is a Borsh-encoded `Hop`: the acting account,
/// the peer account, the hop kind, and the human sentence. The app draws the whole actor map from
/// these events alone (the kind gives the arrow, the accounts give its ends), so it no longer needs
/// the per-actor state diff for the map.
const HOP: [u8; 8] = *b"acv_hop_";

/// Which kind of hop an event records. One byte on the wire (Borsh enum discriminant).
#[derive(BorshSerialize, BorshDeserialize)]
enum HopKind {
    SyncRequest = 0,    // this actor sent a synchronous request to `peer`
    SyncReply = 1,      // `peer` answered; this actor runs again to handle the reply
    AsyncSent = 2,      // this actor sent an asynchronous message to `peer`
    AsyncReceived = 3,  // an asynchronous message from `peer` arrived at this actor
}

/// One hop of a conversation, as carried in a `HOP` event's data (Borsh):
/// `actor` [32] ++ `peer` [32] ++ `kind` [1 byte] ++ `text` (u32-LE length ++ UTF-8 bytes).
#[derive(BorshSerialize, BorshDeserialize)]
struct Hop {
    actor: AccountId,
    peer: AccountId,
    kind: HopKind,
    text: String,
}
/// Marks the request a reply belongs to, in this actor's state, between the two runs.
const PENDING: &str = "#pending min=";
const KEEP_LINES: usize = 8;

lee_core::actor_entrypoint!(raw handle_message);

fn short(id: &AccountId) -> String {
    id.value()[..4].iter().map(|b| format!("{b:02x}")).collect::<String>() + "…"
}

fn hop(actor: AccountId, peer: AccountId, kind: HopKind, text: String) -> ProgramEvent {
    ProgramEvent { selector: HOP, data: borsh::to_vec(&Hop { actor, peer, kind, text }).expect("a hop serializes") }
}

/// The state's lines, without a pending marker.
fn lines(state: &[u8]) -> Vec<String> {
    String::from_utf8_lossy(state)
        .lines()
        .filter(|l| !l.starts_with(PENDING))
        .map(str::to_owned)
        .collect()
}

fn with_line(state: &[u8], line: String) -> Vec<u8> {
    let mut all = lines(state);
    all.push(line);
    let start = all.len().saturating_sub(KEEP_LINES);
    all[start..].join("\n").into_bytes()
}

fn pending_min(state: &[u8]) -> Option<u128> {
    String::from_utf8_lossy(state)
        .lines()
        .find_map(|l| l.strip_prefix(PENDING).and_then(|n| n.parse().ok()))
}

fn ask(input: &ReceiveInput, of: AccountId, min: u128) -> Response {
    let what = if min == 0 { "its balance".to_owned() } else { format!("its balance (must be >= {min})") };
    let mut state = String::from_utf8(with_line(&input.pre_state, format!("-> asked the native token on {} for {what} (sync ReadState)", short(&of)))).expect("utf-8");
    state.push_str(&format!("\n{PENDING}{min}"));
    Response::set_state(state.into_bytes())
        .send_sync(Actor::native_balance(of), &native_token::Message::ReadState)
        .event(hop(input.receiver.account_id, of, HopKind::SyncRequest, format!(
            "actor_conversation on {} asks the native token on {} for {what}: a synchronous ReadState, answered in this same transaction",
            short(&input.receiver.account_id),
            short(&of)
        )))
}

fn on_reply(input: &ReceiveInput, reply: &StateReply) -> Response {
    let of = input.from.expect("a reply has a sender").account_id;
    let balance = decode_balance(&reply.state).expect("a native balance decodes");
    let min = pending_min(&input.pre_state).expect("a reply answers a pending request");
    assert!(
        balance >= min,
        "the balance of {} is {balance}, below the required {min}: the whole transaction is undone, including the request's pending note",
        short(&of)
    );
    Response::set_state(with_line(&input.pre_state, format!("<- StateReply from the native token on {}: balance {balance}", short(&of))))
        .event(hop(input.receiver.account_id, of, HopKind::SyncReply, format!(
            "the native token on {} replied with a StateReply (balance {balance}); actor_conversation runs again to handle it and records the balance",
            short(&of)
        )))
}

fn handle_message(input: &ReceiveInput) -> Response {
    if input.from.is_some_and(|from| from.program_account_id == NATIVE_TOKEN_PROGRAM_ID) {
        let reply: StateReply = borsh::from_slice(&input.message).expect("a native-token reply decodes");
        return on_reply(input, &reply);
    }
    let me = input.receiver;
    match borsh::from_slice::<Message>(&input.message).expect("an actor_conversation message decodes") {
        Message::AskBalance { of } => ask(input, of, 0),
        Message::Require { of, min } => ask(input, of, min),
        Message::Ping { to, note } => {
            assert!(to != me.account_id, "ping another account");
            let text = String::from_utf8_lossy(&note).into_owned();
            Response::set_state(with_line(&input.pre_state, format!("-> sent Pong \"{text}\" to actor_conversation on {} (async)", short(&to))))
                .send_async(Actor::new(to, me.program_account_id), &Message::Pong { note })
                .event(hop(me.account_id, to, HopKind::AsyncSent, format!(
                    "actor_conversation on {} sends Pong to actor_conversation on {} asynchronously: it runs after the synchronous work",
                    short(&me.account_id),
                    short(&to)
                )))
        }
        Message::Pong { note } => {
            assert!(input.from_own_program(), "only actor_conversation sends Pong");
            let from = input.from.expect("from_own_program implies a sender").account_id;
            let text = String::from_utf8_lossy(&note).into_owned();
            Response::set_state(with_line(&input.pre_state, format!("<- Pong \"{text}\" from actor_conversation on {} (async: ran after the sync work, same transaction)", short(&from))))
                .event(hop(me.account_id, from, HopKind::AsyncReceived, format!(
                    "Pong from {} arrives at actor_conversation on {}, after the synchronous work",
                    short(&from),
                    short(&me.account_id)
                )))
        }
    }
}
