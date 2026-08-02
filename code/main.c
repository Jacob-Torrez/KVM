#include "kvm.h"

int main(){
    struct vm vm;
    struct vcpu vcpu;

    if (vm_init(&vm, 1024 * 1024 * 256) == -1){
        return -1;
    }

    if (vcpu_init(&vm, &vcpu) == -1){
        return -1;
    }

    if (load_bzimage(&vm, "bzImage") == -1){
        return -1;
    }
    
    if (setup_regs(&vcpu) == -1){
        return -1;
    }

    if (cpuid_init(&vm, &vcpu) == -1){
        return -1;
    }

    if (vm_run(&vm, &vcpu) == -1){
        return -1;
    }

    return 0;
}