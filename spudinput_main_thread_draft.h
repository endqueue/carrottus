/*
 * spudinput_main_thread_draft.h - on GameController, a gamepad's outputs
 * belong to the main thread, as its poll does.
 *
 * DRAFT, 2026-11-08 (a caller's rumble session, answer 7). APPROVED by the user 2026-11-08 by standing instruction ("do all
 * that is recommended", the session running unattended); in
 * ../claude_made_decisions.md, open for review. Not compilable: it names
 * what spudinput.h declares.
 *
 * Why: the threading pass of 2026-10-28 (../claude_made_decisions.md,
 * "Where the code differs from X3") found spudinput_gamepad_set_rumble and
 * spudinput_gamepad_set_motion_enabled not confined to the main thread on
 * GameController, while the framework's handlers write the pad's state
 * there, and its GCController and haptics engines are the main thread's.
 * spudinput_poll already refuses another thread; these two now say so and
 * do the same. Other native APIs keep taking them from any thread.
 *
 * ABI: none (a Thread: line and a result the calls could already return).
 * Tests: the backend's tests on the Mac call each from a second thread and
 * expect SPUDRESULT_GENERAL_FAILURE, nothing changed (written, not run:
 * this machine is Windows; the change is written, not compiled).
 * Compared: Apple's GCController documentation (main-thread handlers),
 * SDL3's joystick API (its own lock, which SpudLib doesn't take: no lock is
 * hidden in SpudLib).
 */

/* [CHANGED] spudinput_gamepad_set_rumble and
 * spudinput_gamepad_set_motion_enabled gain:
 *
 *  Thread: with SPUDINPUT_NATIVE_API_GAMECONTROLLER, only on the process's
 *  main thread, as spudinput_poll: the framework's controllers and haptics
 *  belong to it. A call on any other thread returns
 *  SPUDRESULT_GENERAL_FAILURE and changes nothing. Other native APIs take
 *  it from any thread.
 *
 *  @retval SPUDRESULT_GENERAL_FAILURE On GameController, called off the
 *          main thread.
 */
