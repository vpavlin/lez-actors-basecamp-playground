# Example programs (prebuilt guests)

These are the public examples from LEZ's program-deployment tutorial (`examples/program_deployment`),
built at LEZ `dev` commit `f6afdd285d274b3406b93877b16d85fd849c2df3` with
`cargo risczero build --manifest-path examples/program_deployment/methods/guest/Cargo.toml`.
They're embedded into the core library (see `embed_examples.cmake`), so the app can deploy them to
its local chain without a RISC Zero toolchain. `SHA256SUMS` lists the exact bytes.

`actor_conversation.bin` is ours, not LEZ's (rebuilt 2026-10-26 at LEZ `f6afdd28` with `patches/0001`, for
the structured `acv_hop_` events): built from `src/actor_conversation.rs` the same way
(link it into LEZ's guest package as `methods/guest/src/bin/actor_conversation.rs`, then
`cargo build --release -p example_program_deployment_methods`). It shows actors talking: a sync
request/reply with the native token, a rollback, and async messages to a declared and an undeclared
actor.

`tail_call_with_pda` is left out: calling it needs PDA admission evidence, which core_ffi's C API
doesn't offer at this commit (docs/LEZ-V04-NOTES.md §4).

To rebuild after moving the LEZ pin: build the guests as above, copy the four `.bin` files here,
and regenerate `SHA256SUMS`.
