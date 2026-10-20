// From LEZ (lee/state_machine/core/src/native_token.rs at dev f6afdd28), the native token program, tests
// left out. Shown in the playground for the ReadState / Transfer / Credit handling.
use borsh::{BorshDeserialize, BorshSerialize};

use crate::{
    account::{AccountId, Actor, ActorState, Balance},
    program::{PdaSeed, ReceiveInput, Response, StateReply, SyncMessage, Transition},
};

/// Hardcoded native token program address.
pub const NATIVE_TOKEN_PROGRAM_ID: AccountId = AccountId::new([0; 32]);

#[derive(Debug, Clone, PartialEq, Eq, BorshSerialize, BorshDeserialize)]
pub enum Message {
    Transfer { to: AccountId, amount: Balance },
    Credit(Balance),
    ReadState,
}

#[derive(Debug, thiserror::Error, Clone, Copy, PartialEq, Eq)]
pub enum TransferError {
    #[error("native transfer message does not decode")]
    InvalidMessage,
    #[error("native transfer sender {account_id} is not authorized")]
    UnauthorizedSender { account_id: AccountId },
    #[error("native credit to {account_id} was not sent by a native transfer")]
    ForeignCredit { account_id: AccountId },
    #[error(transparent)]
    InvalidBalance(#[from] InvalidBalanceEncoding),
    #[error("sender {account_id} holds less than the transferred amount")]
    InsufficientBalance { account_id: AccountId },
    #[error("recipient {account_id} balance overflows")]
    BalanceOverflow { account_id: AccountId },
    #[error("native balance {account_id} was read with no one to reply to")]
    UnroutableRead { account_id: AccountId },
}

#[derive(Debug, thiserror::Error, Clone, Copy, PartialEq, Eq)]
#[error("native balance actor state is not a canonical encoding")]
pub struct InvalidBalanceEncoding;

#[must_use]
pub fn encode_balance(balance: Balance) -> ActorState {
    if balance == 0 {
        ActorState::empty()
    } else {
        ActorState::from(balance.to_le_bytes().to_vec())
    }
}

pub fn decode_balance(data: &[u8]) -> Result<Balance, InvalidBalanceEncoding> {
    if data.is_empty() {
        return Ok(0);
    }
    let bytes = <[u8; 16]>::try_from(data)
        .ok()
        .ok_or(InvalidBalanceEncoding)?;
    match Balance::from_le_bytes(bytes) {
        0 => Err(InvalidBalanceEncoding),
        balance => Ok(balance),
    }
}

pub fn handle_message(input: &ReceiveInput) -> Result<Transition, TransferError> {
    let Ok(message) = borsh::from_slice::<Message>(&input.message) else {
        return Err(TransferError::InvalidMessage);
    };
    let account_id = input.receiver.account_id;
    let response = match message {
        Message::Transfer { to, amount } => {
            debit(input, amount)?.send_async(Actor::native_balance(to), &Message::Credit(amount))
        }
        Message::Credit(amount) => {
            if !input.from_own_program() {
                return Err(TransferError::ForeignCredit { account_id });
            }
            let post = decode_balance(&input.pre_state)?
                .checked_add(amount)
                .ok_or(TransferError::BalanceOverflow { account_id })?;
            Response::set_state(encode_balance(post))
        }
        Message::ReadState => {
            let reader = input
                .from
                .ok_or(TransferError::UnroutableRead { account_id })?;
            Response::keep_state().send_sync(reader, &StateReply::from(input))
        }
    };
    Ok(response.into_transition(input.clone()))
}

fn debit(input: &ReceiveInput, amount: Balance) -> Result<Response, TransferError> {
    let account_id = input.receiver.account_id;
    if !input.is_authorized {
        return Err(TransferError::UnauthorizedSender { account_id });
    }
    let post = decode_balance(&input.pre_state)?
        .checked_sub(amount)
        .ok_or(TransferError::InsufficientBalance { account_id })?;
    Ok(Response::set_state(encode_balance(post)))
}

/// A transfer out of an account the caller holds under `seed`.
#[must_use]
pub fn custody_transfer(
    from: AccountId,
    seed: PdaSeed,
    to: AccountId,
    amount: Balance,
) -> SyncMessage {
    SyncMessage::new(
        Actor::native_balance(from),
        &Message::Transfer { to, amount },
    )
    .with_pda_seeds(vec![seed])
}

