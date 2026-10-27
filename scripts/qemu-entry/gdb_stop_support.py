"""Pure state helpers for the QEMU GDB stop observer."""


def finish_stop_action(pc, sp, return_target, return_stack, trace_remaining):
    """Classify a stop while waiting for a function's original stack return."""
    if pc == return_target:
        return 'function-return' if sp == return_stack else 'other-stack-return'
    if pc in trace_remaining:
        return 'tracepoint'
    return 'unexpected-stop'
