#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"

static void resetStack(au3VM *vm)
{
    vm->top = vm->stack;
}

au3VM *au3_create()
{
    au3VM *vm = calloc(sizeof(au3VM), 1);

    if (vm != NULL) {

        resetStack(vm);
    }

    return vm;
}

void au3_close(au3VM *vm)
{
    if (vm == NULL) return;

    free(vm);
}

#define PUSH(vm, v)     *((vm)->top++) = (v)
#define POP(vm)         *(--(vm)->top)

static au3Status execute(au3VM *vm)
{
#define READ_BYTE()     (*vm->ip++)
#define READ_LAST()     (vm->ip[-1])
#define READ_CONST()    (vm->chunk->constants.values[READ_BYTE()])

#define DISPATCH()      for (;;) switch (READ_BYTE())
#define CASE_CODE(x)    case OP_##x:
#define CASE_ERROR()    default:
#define NEXT            continue

    DISPATCH() {
        CASE_CODE(RET) {

            return AU3_OK;
        }
        CASE_CODE(CONST) {
            au3Value value = READ_CONST();
            PUSH(vm, value);
            NEXT;
        }
        CASE_ERROR() {
            printf("Bad opcode, got %d!\n", READ_LAST());
            return AU3_RUNTIME_ERROR;
        }
    }

    return AU3_OK;
}

au3Status au3_interpret(au3VM *vm, const char *source)
{
    au3Chunk chunk;
    au3_initChunk(&chunk);

    vm->chunk = &chunk;
    vm->ip = vm->chunk->code;
    au3Status result = execute(vm);

    au3_freeChunk(&chunk);
    return AU3_OK;
}