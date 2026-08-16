#include "kvm.h"

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

    if (ioctl(vm->fd, KVM_CREATE_IRQCHIP, 0) == -1){
        perror("KVM CREATE IRQCHIP");
        return -1;
    }

    struct kvm_pit_config pit_config = { .flags = 0 };
    if (ioctl(vm->fd, KVM_CREATE_PIT2, &pit_config) == -1){
        perror("KVM CREATE PIT2");
        return -1;
    }

    vm->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (vm->mem == MAP_FAILED){
        printf("VM memory map failed\n");
        return -1;
    }
    vm->mem_size = mem_size;

    struct kvm_userspace_memory_region memreg;
    memreg.slot = 0;
    memreg.flags = 0;
    memreg.guest_phys_addr = 0;
    memreg.memory_size = mem_size;
    memreg.userspace_addr = (uint64_t)vm->mem;
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

static void sigalrm_handler(int sig) {
    (void)sig;
}

int vm_run(struct vm* vm, struct vcpu* vcpu){
    uint8_t ier, lcr, mcr, scratch;
    ier = lcr = mcr = scratch = 0;

    signal(SIGALRM, sigalrm_handler);
    siginterrupt(SIGALRM, 1);

    struct itimerval timer = {
        .it_value = { .tv_sec = 0, .tv_usec = 10000 },
        .it_interval = { .tv_sec = 0, .tv_usec = 10000},
    };
    setitimer(ITIMER_REAL, &timer, NULL);

    for(;;){
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 0};
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0 && (ier & 0x1)) {
            struct kvm_irq_level irq_level = { .irq = 4, .level = 1 };
            ioctl(vm->fd, KVM_IRQ_LINE, &irq_level);
            irq_level.level = 0;
            ioctl(vm->fd, KVM_IRQ_LINE, &irq_level);
        }

        if (ioctl(vcpu->fd, KVM_RUN, 0) == -1){
            if (errno = EINTR) continue;
            perror("KVM RUN");
            return -1;
        }

        switch (vcpu->run->exit_reason){
            case KVM_EXIT_HLT: {
                // dump_regs(vcpu);
                continue;
            }

            case KVM_EXIT_IO:
                fprintf(stderr, "[IO] %s port=%#x size=%u data=%#x\n",
                    vcpu->run->io.direction == KVM_EXIT_IO_OUT ? "OUT" : "IN",
                    vcpu->run->io.port,
                    vcpu->run->io.size,
                    vcpu->run->io.direction == KVM_EXIT_IO_OUT
                        ? *((unsigned char*)vcpu->run + vcpu->run->io.data_offset)
                        : 0);

                if (vcpu->run->io.direction == KVM_EXIT_IO_OUT && vcpu->run->io.port == 0x3F8) {
                    fwrite((char*)vcpu->run + vcpu->run->io.data_offset, vcpu->run->io.size, 1, stdout);
                    fflush(stdout);
                    if (ier & 0x02) {
                        struct kvm_irq_level irq_level = { .irq = 4, .level = 1};
                        ioctl(vm->fd, KVM_IRQ_LINE, &irq_level);
                        irq_level.level = 0;
                        ioctl(vm->fd, KVM_IRQ_LINE, &irq_level);
                    }
                    continue;
                } 
                else if (vcpu->run->io.direction == KVM_EXIT_IO_IN && vcpu->run->io.port == 0x3FD){
                    fd_set fds;
                    FD_ZERO(&fds);
                    FD_SET(STDIN_FILENO, &fds);
                    struct timeval tv = {0, 0};
                    int has_data = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;

                    *((char*)vcpu->run + vcpu->run->io.data_offset) = has_data ? 0x61 : 0x60;
                    continue;
                }
                else if (vcpu->run->io.direction == KVM_EXIT_IO_IN && vcpu->run->io.port == 0x3F8){
                    char c = 0;
                    read(STDIN_FILENO, &c, 1);
                    fprintf(stderr, "[DEBUG] sending bytes 0x%02x to guest", (unsigned char)c);
                    *((char*)vcpu->run + vcpu->run->io.data_offset) = c;
                    continue;
                }
                else if (vcpu->run->io.direction == KVM_EXIT_IO_OUT && vcpu->run->io.port == 0x3F9){
                    uint8_t new_ier = *((char*)vcpu->run + vcpu->run->io.data_offset);

                    if ((new_ier & 0x02) && !(ier & 0x02)) {
                        struct kvm_irq_level irq_level = { .irq = 4, .level = 1};
                        ioctl(vm->fd, KVM_IRQ_LINE, &irq_level);
                        irq_level.level = 0;
                        ioctl(vm->fd, KVM_IRQ_LINE, &irq_level);
                    }

                    ier = new_ier;
                    continue;
                }
                else if (vcpu->run->io.direction == KVM_EXIT_IO_IN && vcpu->run->io.port == 0x3F9){
                    *((char*)vcpu->run + vcpu->run->io.data_offset) = ier;
                    continue;
                }
                else if (vcpu->run->io.direction == KVM_EXIT_IO_IN && vcpu->run->io.port == 0x3FA){
                    fd_set fds;
                    FD_ZERO(&fds);
                    FD_SET(STDIN_FILENO, &fds);
                    struct timeval tv = {0, 0};
                    int rx_ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;

                    uint8_t iir;
                    if (rx_ready && (ier & 0x01))
                        iir = 0x04;
                    else if (ier & 0x02)
                        iir = 0x02;
                    else
                        iir = 0x01;

                    *((char*)vcpu->run + vcpu->run->io.data_offset) = iir;
                    continue;
                }
                else if (vcpu->run->io.port == 0x3FB){
                    if (vcpu->run->io.direction == KVM_EXIT_IO_OUT)
                        lcr = *((char*)vcpu->run + vcpu->run->io.data_offset);
                    else
                        *((char*)vcpu->run + vcpu->run->io.data_offset) = lcr;
                    continue;
                }
                else if (vcpu->run->io.port == 0x3FC){
                    if (vcpu->run->io.direction == KVM_EXIT_IO_OUT)
                        mcr = *((char*)vcpu->run + vcpu->run->io.data_offset);
                    else
                        *((char*)vcpu->run + vcpu->run->io.data_offset) = mcr;
                    continue;
                }
                else if (vcpu->run->io.port == 0x3FE && vcpu->run->io.direction == KVM_EXIT_IO_IN){
                    *((char*)vcpu->run + vcpu->run->io.data_offset) = 0xB0; // DCD+CTS+DSR set
                    continue;
                }
                else if (vcpu->run->io.port == 0x3FF){
                    if (vcpu->run->io.direction == KVM_EXIT_IO_OUT)
                        scratch = *((char*)vcpu->run + vcpu->run->io.data_offset);
                    else
                        *((char*)vcpu->run + vcpu->run->io.data_offset) = scratch; // must echo back what was written
                    continue;
                }
            
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

    size_t initramfs_size;
    if (load_initramfs(vm, &initramfs_size) == -1){
        printf("failed to load initramfs\n");
        return -1;
    }

    boot.hdr.ramdisk_image = 0x4000000;
    boot.hdr.ramdisk_size = initramfs_size;

    /* 0x9800 as suggested by boot.txt */
    boot.hdr.heap_end_ptr = 0x9800 - 0x200;

    boot.hdr.cmd_line_ptr = 0x90000 + 0x9800;
    strcpy((char*)vm->mem + 0x90000 + 0x9800, "console=ttyS0,115200 earlyprintk=ttyS0 ignore_loglevel loglevel=8 nokaslr");

    boot.hdr.hardware_subarch = 0;

    boot.e820_table[0].addr = 0;
    boot.e820_table[0].size = 0x09FC00;
    boot.e820_table[0].type = 1; // E820_RAM

    boot.e820_table[1].addr = 0x09FC00;
    boot.e820_table[1].size = 0x100000 - 0x09FC00;
    boot.e820_table[1].type = 2; // E820_RESERVED

    boot.e820_table[2].addr = 0x100000;
    boot.e820_table[2].size = vm->mem_size - 0x100000;
    boot.e820_table[2].type = 1;

    boot.e820_entries = 3;

    /* Load boot_params at 0x10000 in guest memory */
    memcpy((char*)vm->mem + 0x10000, &boot, sizeof(struct boot_params));

    fclose(bzimage);

    return 0;
}

int load_initramfs(struct vm* vm, size_t* out_size){
    FILE* initramfs = fopen("initramfs.cpio.gz", "rb");
    if (initramfs == NULL){
        printf("failed to open initramfs\n");
        return -1;
    }

    if (fseek(initramfs, 0, SEEK_END)){
        printf("seek end 0 fail\n");
        return -1;
    }

    long initramfs_size = ftell(initramfs);

    if (fseek(initramfs, 0, SEEK_SET)){
        printf("seek set 0 fail\n");
        return -1;
    }

    if (fread((char*)vm->mem + 0x4000000, 1, initramfs_size, initramfs) != initramfs_size){
        printf("failed to load initramfs\n");
        return -1;
    }

    fclose(initramfs);
    *out_size = (size_t)initramfs_size;
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