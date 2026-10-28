"""Pure state helpers for the QEMU GDB stop observer."""


def parse_indirect_memory_request(request):
    """Parse one bounded pointer-field read, returning address, offset, length."""
    try:
        fields = request.split(':')
        if len(fields) != 3:
            raise ValueError
        address, offset, length = (int(field, 16) for field in fields)
    except ValueError as error:
        raise ValueError('Indirect reads use POINTER_ADDRESS:HEX_OFFSET:HEX_LENGTH') from error
    if not 0 <= address < 1 << 64 or address + 8 > 1 << 64:
        raise ValueError('Indirect pointer-field address must fit in 64 bits')
    if not 0 <= offset < 1 << 64 or not 1 <= length <= 4096:
        raise ValueError('Indirect reads require a nonnegative 64-bit offset and 1..4096 bytes')
    if offset + length > 1 << 64:
        raise ValueError('Indirect offset and length overflow 64 bits')
    return address, offset, length


def parse_trace_register_memory_request(request):
    """Parse one bounded trace-time read through a saved x0-x30 register."""
    try:
        pc_text, register, offset_text, length_text = request.split(':')
        pc = int(pc_text, 16)
        offset = int(offset_text, 16)
        length = int(length_text, 16)
        if not register.startswith('x') or not register[1:].isdigit():
            raise ValueError
        register_index = int(register[1:])
    except ValueError as error:
        raise ValueError('Trace reads use HEX_PC:x0-x30:HEX_OFFSET:HEX_LENGTH') from error
    if not 0 <= pc < 1 << 64 or pc & 3:
        raise ValueError('Trace read PC must be an aligned 64-bit address')
    if not 0 <= register_index <= 30:
        raise ValueError('Trace read register must be x0 through x30')
    if not 0 <= offset < 1 << 64 or not 1 <= length <= 4096:
        raise ValueError('Trace reads require a nonnegative 64-bit offset and 1..4096 bytes')
    if offset + length > 1 << 64:
        raise ValueError('Trace read offset and length overflow 64 bits')
    return pc, register_index, offset, length


def finish_stop_action(pc, sp, return_target, return_stack, trace_remaining):
    """Classify a stop while waiting for a function's original stack return."""
    if pc == return_target:
        return 'function-return' if sp == return_stack else 'other-stack-return'
    if pc in trace_remaining:
        return 'tracepoint'
    return 'unexpected-stop'
