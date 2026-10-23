# app_hello

Owns the hello counter, celebration timer and GSP event callback. It is the same
controller on device and Host. Depends only on the public ESP-GSP API and the
bindings generated from `ui/main.json`; it never imports display, BSP or `main`.
CMake generates the bundle for this component; generated bindings stay private.
Authored scenes and fonts live in `ui/`. The optional desktop adapter and its
standalone CMake entry live in `host/`. The ESP-IDF component registers only
`app_hello.c`; it never compiles the Host adapter. Both builds use `include/`
and the same controller implementation, so UI behavior has one source.

```c
app_hello_handle_t controller = NULL;
esp_gsp_err_t err = app_hello_create(ui, &controller);
/* If err == ESP_GSP_OK, keep ui/controller alive while GSP dispatches events. */
```

The controller borrows `ui`, claims its one application event callback, and owns
its private state and timer. `ui` must have no other application event owner.
Create once from the startup task (Host: before polling), with no concurrent
create/destroy. Callbacks may start as soon as registration accepts the initialized
context, even before create returns. Other tasks should deliver events, not mutate fields.
Diagnostic snapshots are value copies, read on that loop or after it is quiescent.
Reads do not allocate, lock or wait.

Create returns `ESP_GSP_ERR_INVALID_ARG` for NULL arguments, `ESP_GSP_ERR_NO_MEM`
for allocation failure, or a GSP setter/registration error. A valid output slot is
cleared on failure; scene setters may already have been applied. The borrowed UI
is never closed by this component.

Host shutdown exits polling before `app_hello_destroy(&controller)` and closes GSP
afterward. The device example keeps both alive until reboot. Before adding device
shutdown, implement an explicit runtime quiescence step; deleting a timer alone
is not a join for a callback already selected for execution. Failed cleanup
retains the handle for retry. The caller must not destroy the borrowed UI first.
Never resume a dispatcher retaining callbacks to a freed controller. Destroy is
not callable from callbacks or concurrently with other operations. A NULL handle
pointer returns `ESP_GSP_ERR_INVALID_ARG`; an already-NULL owned handle succeeds.

GSP errors propagate from startup/cleanup; callback errors are recorded in the
snapshot. Calls add no deadline or retry loop; backend queue/transport timeouts
apply. No functions are ISR-safe. Add behavior here through small private helpers;
keep timer handles and animation bookkeeping out of public headers.
