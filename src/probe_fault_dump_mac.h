#ifndef BB_PROBE_FAULT_DUMP_MAC_H
#define BB_PROBE_FAULT_DUMP_MAC_H

/* Configuration is parsed before installing handlers. The fatal-path helpers use fixed
 * buffers, write(2), and kernel-mediated reads; they never dereference a guest address. */
#define FAULT_DUMP_MAX_EXPRS 16
#define FAULT_DUMP_MAX_OPS 12
#define FAULT_DUMP_LABEL_SIZE 64

enum FaultDumpReg {
    FD_RAX, FD_RBX, FD_RCX, FD_RDX, FD_RSI, FD_RDI,
    FD_R8, FD_R9, FD_R10, FD_R11, FD_R12, FD_R13, FD_R14, FD_R15,
    FD_RIP, FD_RSP, FD_RBP, FD_RFLAGS, FD_IMAGE, FD_REG_COUNT
};
static const char *const fault_dump_reg_names[FD_REG_COUNT] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
    "r12", "r13", "r14", "r15", "rip", "rsp", "rbp", "rflags", "image"
};
enum FaultDumpOpCode { FD_REG, FD_ADD, FD_SUB, FD_DEREF };
typedef struct { enum FaultDumpOpCode kind; uint64_t value; } FaultDumpOp;
typedef struct {
    char label[FAULT_DUMP_LABEL_SIZE];
    FaultDumpOp ops[FAULT_DUMP_MAX_OPS];
    unsigned count;
} FaultDumpExpr;
static FaultDumpExpr fault_dump_exprs[FAULT_DUMP_MAX_EXPRS];
static unsigned fault_dump_expr_count;
static volatile sig_atomic_t fault_dump_enabled;
static mach_port_t fault_dump_task;

static void fault_dump_space(const char **at) {
    while (**at == ' ' || **at == '\t') ++*at;
}
static int fault_dump_op(FaultDumpExpr *expr, enum FaultDumpOpCode kind, uint64_t value) {
    if (expr->count == FAULT_DUMP_MAX_OPS) return 0;
    expr->ops[expr->count++] = (FaultDumpOp){kind, value};
    return 1;
}
static int fault_dump_number(const char **at, uint64_t *number) {
    unsigned base = 10, digits = 0;
    uint64_t value = 0;
    if ((*at)[0] == '0' && ((*at)[1] == 'x' || (*at)[1] == 'X')) {
        base = 16; *at += 2;
    }
    for (;;) {
        unsigned char c = (unsigned char)**at;
        unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                         c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                         c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (digit >= base) break;
        if (value > (UINT64_MAX - digit) / base) return 0;
        value = value * base + digit; ++*at; ++digits;
    }
    *number = value;
    return digits != 0;
}
static int fault_dump_parse_expr(const char **at, FaultDumpExpr *expr, unsigned depth) {
    if (depth > 3) return 0;
    fault_dump_space(at);
    if (**at == '[') {
        ++*at;
        if (!fault_dump_parse_expr(at, expr, depth + 1)) return 0;
        fault_dump_space(at);
        if (**at != ']') return 0;
        ++*at;
        if (!fault_dump_op(expr, FD_DEREF, 0)) return 0;
    } else {
        char name[8]; unsigned n = 0;
        while ((**at >= 'a' && **at <= 'z') || (**at >= 'A' && **at <= 'Z') ||
               (**at >= '0' && **at <= '9')) {
            if (n == sizeof(name) - 1) return 0;
            char c = *(*at)++;
            name[n++] = c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c;
        }
        name[n] = 0;
        unsigned reg = 0;
        while (reg < FD_REG_COUNT && strcmp(name, fault_dump_reg_names[reg])) ++reg;
        if (reg == FD_REG_COUNT || !fault_dump_op(expr, FD_REG, reg)) return 0;
    }
    fault_dump_space(at);
    while (**at == '+' || **at == '-') {
        enum FaultDumpOpCode op = *(*at)++ == '+' ? FD_ADD : FD_SUB;
        fault_dump_space(at);
        uint64_t number;
        if (!fault_dump_number(at, &number) || !fault_dump_op(expr, op, number)) return 0;
        fault_dump_space(at);
    }
    return 1;
}
static int fault_dump_configure(const char *setting) {
    fault_dump_enabled = 0;
    fault_dump_expr_count = 0;
    if (!setting || !*setting || !strcmp(setting, "0")) return 0;
    if (strcmp(setting, "1")) {
        const char *at = setting;
        for (;;) {
            if (fault_dump_expr_count == FAULT_DUMP_MAX_EXPRS) return -1;
            const char *start = at;
            FaultDumpExpr *expr = &fault_dump_exprs[fault_dump_expr_count];
            expr->count = 0;
            if (!fault_dump_parse_expr(&at, expr, 0)) return -1;
            size_t size = (size_t)(at - start);
            if (!size || size >= sizeof(expr->label)) return -1;
            memcpy(expr->label, start, size); expr->label[size] = 0;
            ++fault_dump_expr_count;
            if (!*at) break;
            if (*at++ != ',' || !*at) return -1;
        }
    }
    fault_dump_task = mach_task_self();
    fault_dump_enabled = 1;
    return 0;
}

static void fault_dump_write(const char *data, size_t size) {
    while (size) {
        ssize_t written = write(2, data, size);
        if (written > 0) { data += written; size -= (size_t)written; }
        else if (written < 0 && errno == EINTR) continue;
        else break;
    }
}
static char *fault_dump_text(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}
static char *fault_dump_hex(char *out, uint64_t value) {
    const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) *out++ = digits[(value >> (60 - 4 * i)) & 15];
    return out;
}
static kern_return_t fault_dump_read(uint64_t address, void *data, size_t size) {
    if (size && address > UINT64_MAX - (size - 1)) return KERN_INVALID_ADDRESS;
    mach_vm_size_t got = 0;
    kern_return_t result = mach_vm_read_overwrite(fault_dump_task, address, size,
                                                 (mach_vm_address_t)data, &got);
    return result == KERN_SUCCESS && got != size ? KERN_INVALID_ADDRESS : result;
}
static int fault_dump_evaluate(const FaultDumpExpr *expr, const uint64_t regs[FD_REG_COUNT],
                               uint64_t *address, kern_return_t *error) {
    uint64_t value = 0;
    for (unsigned i = 0; i < expr->count; ++i) {
        const FaultDumpOp *op = &expr->ops[i];
        switch (op->kind) {
        case FD_REG: value = regs[op->value]; break;
        case FD_ADD:
            if (value > UINT64_MAX - op->value) return 0;
            value += op->value; break;
        case FD_SUB:
            if (value < op->value) return 0;
            value -= op->value; break;
        case FD_DEREF:
            *error = fault_dump_read(value, &value, sizeof(value));
            if (*error != KERN_SUCCESS) return 0;
            break;
        }
    }
    *address = value;
    return 1;
}
static int fault_dump_guest_pointer(uint64_t value) {
    return value >= UINT64_C(0x0800000000) && value < UINT64_C(0x10000000000);
}
static void fault_dump_memory(const char *label, uint64_t address, unsigned size) {
    char line[160];
    char *end = fault_dump_text(line, "BB_FAULT_DUMP memory ");
    end = fault_dump_text(end, label); end = fault_dump_text(end, " address=0x");
    end = fault_dump_hex(end, address); *end++ = '\n';
    fault_dump_write(line, (size_t)(end - line));
    for (unsigned offset = 0; offset < size; offset += 16) {
        unsigned char bytes[16];
        if (address > UINT64_MAX - offset) {
            end = fault_dump_text(line, "  address overflow\n");
            fault_dump_write(line, (size_t)(end - line));
            break;
        }
        kern_return_t error = fault_dump_read(address + offset, bytes, sizeof(bytes));
        end = fault_dump_text(line, "  0x"); end = fault_dump_hex(end, address + offset);
        if (error != KERN_SUCCESS) {
            end = fault_dump_text(end, " unreadable mach=0x");
            end = fault_dump_hex(end, (uint64_t)(unsigned)error);
        } else {
            const char digits[] = "0123456789abcdef";
            *end++ = ':';
            for (unsigned i = 0; i < sizeof(bytes); ++i) {
                *end++ = ' '; *end++ = digits[bytes[i] >> 4]; *end++ = digits[bytes[i] & 15];
            }
        }
        *end++ = '\n'; fault_dump_write(line, (size_t)(end - line));
    }
}
static void fault_dump_guest(ucontext_t *context, uintptr_t image_base, int signal_number,
                             uintptr_t fault_address) {
    const x86_thread_state64_t *ss = &context->uc_mcontext->__ss;
    const uint64_t regs[FD_REG_COUNT] = {
        ss->__rax, ss->__rbx, ss->__rcx, ss->__rdx, ss->__rsi, ss->__rdi,
        ss->__r8, ss->__r9, ss->__r10, ss->__r11, ss->__r12, ss->__r13, ss->__r14, ss->__r15,
        ss->__rip, ss->__rsp, ss->__rbp, ss->__rflags, image_base
    };
    char line[160];
    char *end = fault_dump_text(line, "Guest fault BB_FAULT_DUMP signal=0x");
    end = fault_dump_hex(end, (uint64_t)(unsigned)signal_number);
    end = fault_dump_text(end, " guest-offset=0x"); end = fault_dump_hex(end, regs[FD_RIP] - image_base);
    end = fault_dump_text(end, " address=0x"); end = fault_dump_hex(end, fault_address);
    *end++ = '\n'; fault_dump_write(line, (size_t)(end - line));
    for (unsigned i = 0; i < FD_IMAGE; ++i) {
        end = fault_dump_text(line, "BB_FAULT_DUMP ");
        end = fault_dump_text(end, fault_dump_reg_names[i]);
        end = fault_dump_text(end, "=0x"); end = fault_dump_hex(end, regs[i]);
        *end++ = '\n'; fault_dump_write(line, (size_t)(end - line));
    }
    for (unsigned i = 0; i < FD_IMAGE; ++i) {
        if (fault_dump_guest_pointer(regs[i]))
            fault_dump_memory(fault_dump_reg_names[i], regs[i] - 64, 128);
    }
    fault_dump_memory("rsp-stack (256 bytes)", regs[FD_RSP], 256);
    for (unsigned i = 0; i < fault_dump_expr_count; ++i) {
        uint64_t address = 0;
        kern_return_t error = KERN_SUCCESS;
        if (fault_dump_evaluate(&fault_dump_exprs[i], regs, &address, &error)) {
            fault_dump_memory(fault_dump_exprs[i].label, address, 128);
        } else {
            end = fault_dump_text(line, "BB_FAULT_DUMP expression ");
            end = fault_dump_text(end, fault_dump_exprs[i].label);
            end = fault_dump_text(end, error == KERN_SUCCESS ? " address overflow" : " unreadable dereference mach=0x");
            if (error != KERN_SUCCESS) end = fault_dump_hex(end, (uint64_t)(unsigned)error);
            *end++ = '\n'; fault_dump_write(line, (size_t)(end - line));
        }
    }
}
#endif
