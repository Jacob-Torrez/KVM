#include "kvm.h"
#include <errno.h>

int vm_init(struct vm* vm, size_t mem_size){
    vm->sys_fd = open("/dev/kvm", O_RDWR);
    if (vm->sys_fd == -1){
        printf("Failed to open /dev/kvm\n");
        return -1;
    }


    vm->fd = ioctl(vm->sys_fd, KVM_CREATE_VM, 0);
    if (vm->fd == -1){
        perror("KVM CREATE VM");
        return -1;
    }


    vm->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (vm->mem == MAP_FAILED){
        printf("VM memory map failed\n");
        return -1;
    }

    struct kvm_userspace_memory_region memreg;
    memreg.slot = 0;
    memreg.flags = 0;
    memreg.guest_phys_addr = 0;
    memreg.memory_size = mem_size;
    memreg.userspace_addr = vm->mem;
    if (ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION, &memreg) == -1){
        perror("KVM SET USER MEMORY REGION");
        return -1;
    }

    if (ioctl(vm->fd, KVM_SET_TSS_ADDR, 0xffffd000) == -1){
        perror("KVM SET TSS ADDR");
        return -1;
    }

    return 0;
}

int vcpu_init(struct vm* vm, struct vcpu* vcpu){
    vcpu->fd = ioctl(vm->fd, KVM_CREATE_VCPU, 0);
    if (vcpu->fd == -1){
        perror("KVM CREATE VCPU");
        return -1;
    }


    int run_size = ioctl(vm->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    vcpu->run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu->fd, 0);
    if (vcpu->run == MAP_FAILED){
        printf("VCPU run map failed\n");
        return -1;
    }

    return 0;
}

int cpuid_init(struct vm* vm, struct vcpu* vcpu){
    struct kvm_cpuid2* cpuid;

    cpuid = calloc(1, sizeof(*cpuid) + 100  * sizeof(*cpuid->entries));
    if (cpuid == NULL){
        perror("CPUID calloc");
        return -1;
    }
    
    cpuid->nent = 100;

    if (ioctl(vm->sys_fd, KVM_GET_SUPPORTED_CPUID, cpuid) == -1){
        perror("KVM GET SUPPORTED CPUID");
        return -1;
    }

    if (ioctl(vcpu->fd, KVM_SET_CPUID2, cpuid) == -1){
        perror("KVM SET CPUID2");
        return -1;
    }

    free(cpuid);
}

int vm_run(struct vm* vm, struct vcpu* vcpu){
    for(;;){
        if (ioctl(vcpu->fd, KVM_RUN, 0) == -1){
            perror("KVM RUN");
            return -1;
        }

        switch (vcpu->run->exit_reason){
            case KVM_EXIT_HLT: {
                dump_regs(vcpu);
                return 0;
            }

            case KVM_EXIT_IO:
                if (vcpu->run->io.direction == KVM_EXIT_IO_OUT && vcpu->run->io.port == 0x3F8) {
                    fwrite((char*)vcpu->run + vcpu->run->io.data_offset, vcpu->run->io.size, 1, stdout);
                    fflush(stdout);
                    continue;
                } else if (vcpu->run->io.direction == KVM_EXIT_IO_IN && vcpu->run->io.port == 0x3FD){
                    *((char*)vcpu->run + vcpu->run->io.data_offset) = 0x60;
                    continue;
                }

                /* printf("IO %s port=%#x size=%u\n",
                    vcpu->run->io.direction == KVM_EXIT_IO_OUT ? "OUT" : "IN",
                    vcpu->run->io.port,
                    vcpu->run->io.size); */
            
            default:
                // printf("not handled %u\n", vcpu->run->exit_reason);
                continue;
        }
    }
}

/* Loads the bzimage into guest memory AND sets up the boot_params struct */
int load_bzimage(struct vm* vm, const char* filename){
    FILE* bzimage = fopen(filename, "rb");
    if (bzimage == NULL){
        printf("failed to open %s\n", filename);
        return -1;
    }

    struct boot_params boot;

    if (fread(&boot, 1, sizeof(boot), bzimage) != sizeof(boot)){
        printf("failed to read setup header\n");
        return -1;
    }

    if (memcmp(&boot.hdr.header, BZMAGIC, strlen(BZMAGIC))){
        printf("setup header missing magic\n");
        return -1;
    }

    if (fseek(bzimage, 0, SEEK_END)){
        printf("seek end 0 fail\n");
        return -1;
    }

    long bzimage_size = ftell(bzimage);

    if (fseek(bzimage, 0, SEEK_SET)){
        printf("seek set 0 fail\n");
        return -1;
    }

    int setup_sects = boot.hdr.setup_sects;

    if (setup_sects == 0){
        setup_sects = 4;
    }

    /* Loads real-mode code at 0x90000 in guest memory */
    if (fread((char*)vm->mem + 0x90000, 1, (setup_sects + 1) * 512, bzimage) != (setup_sects + 1) * 512){
        printf("failed to load real-mode code\n");
        return -1;
    }

    /* Loads the protected-mode code at 0x100000 in guest memory */
    if (fread((char*)vm->mem + 0x100000, 1, bzimage_size - (setup_sects + 1) * 512, bzimage) != bzimage_size - (setup_sects + 1) * 512){
        printf("failed to load protected mode code\n");
        return -1;
    }

    boot.hdr.vid_mode = 0;

    boot.hdr.type_of_loader = 0xFF;

    /* QUIET_FLAG == 0, CAN_USE_HEAP == 1 */
    boot.hdr.loadflags = ((boot.hdr.loadflags & ~0x20) | 0x80);

    /* COME BACK TO CHANGE WHEN DOING INITRAM */
    boot.hdr.ramdisk_image = 0;
    boot.hdr.ramdisk_size = 0;

    /* 0x9800 as suggested by boot.txt */
    boot.hdr.heap_end_ptr = 0x9800 - 0x200;

    boot.hdr.cmd_line_ptr = 0x90000 + 0x9800;
    strcpy((char*)vm->mem + 0x90000 + 0x9800, "console=ttyS0 earlyprintk=serial ignore_loglevel loglevel=8 nokaslr");

    boot.hdr.hardware_subarch = 0;

    /* Load boot_params at 0x10000 in guest memory */
    memcpy((char*)vm->mem + 0x10000, &boot, sizeof(struct boot_params));

    fclose(bzimage);

    return 0;
}

int setup_regs(struct vcpu* vcpu){
    struct kvm_regs regs;
    struct kvm_sregs sregs;

    if (ioctl(vcpu->fd, KVM_GET_REGS, &regs) == -1){
        perror("KVM GET REGS");
        return -1;
    }
    if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) == -1){
        perror("KVM GET SREGS");
        return -1;
    }

    regs.rflags = 0x2;
    regs.rip = 0x100000; // 32-bit kernel entry point
    regs.rsi = 0x10000; // boot_params

    sregs.cs.base = 0; sregs.cs.limit = 0xffffffff; sregs.cs.g = 1;
    sregs.ds.base = 0; sregs.ds.limit = 0xffffffff; sregs.ds.g = 1;
    sregs.es.base = 0; sregs.es.limit = 0xffffffff; sregs.es.g = 1;
    sregs.fs.base = 0; sregs.fs.limit = 0xffffffff; sregs.fs.g = 1;
    sregs.gs.base = 0; sregs.gs.limit = 0xffffffff; sregs.gs.g = 1;
    sregs.ss.base = 0; sregs.ss.limit = 0xffffffff; sregs.ss.g = 1;

    sregs.cs.db = 1;
    sregs.ss.db = 1;
    sregs.cr0 |= 0x1;  // enable protected mode

    if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) == -1){
        perror("KVM SET REGS");
        return -1;
    }
    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) == -1){
        perror("KVM SET SREGS");
        return -1;
    }

    return 0;
}

void dump_regs(struct vcpu* vcpu){
    struct kvm_regs regs;

    ioctl(vcpu->fd, KVM_GET_REGS, &regs);

    printf("finished at rip=0x%llx rsp=0x%llx rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx\n", regs.rip, regs.rsp, regs.rax, regs.rbx, regs.rcx, regs.rdx, regs.rsi, regs.rdi);
}