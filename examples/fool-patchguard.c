/* The LibVMI Library is an introspection library that simplifies access to
 * memory in a target virtual machine or in a file containing a dump of
 * a system's physical memory.  LibVMI is based on the XenAccess Library.
 *
 * Author: Mathieu Tarral (mathieu.tarral@ssi.gouv.fr)
 *
 * This file is part of LibVMI.
 *
 * LibVMI is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * LibVMI is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with LibVMI.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdio.h>
#include <inttypes.h>
#include <signal.h>

#include <bddisasm/disasmtypes.h>
#include <bddisasm/bddisasm.h>

#include <libvmi/libvmi.h>
#include <libvmi/events.h>

// maximum size of an x86 instruction
#define MAX_SIZE_X86_INSN 15

// These definitions are required by libbddisasm
int nd_vsnprintf_s(
    char *buffer,
    size_t sizeOfBuffer,
    size_t count,
    const char *format,
    va_list argptr
    )
{
    (void)count;
    return vsnprintf(buffer, sizeOfBuffer, format, argptr);
}

void* nd_memset(void *s, int c, size_t n)
{
    return memset(s, c, n);
}

// Data struct to be passed as void* to the callback
typedef struct cb_data {
    bool is64;
    addr_t ntload_driver_entry_addr;
    emul_read_t emul_read;
} cb_data_t;


static int interrupted = 0;
static void close_handler(int sig)
{
    interrupted = sig;
}

bool mem_access_size_from_insn(INSTRUX *insn, size_t *size)
{
    // This function returns the memory access size of a given insn

    // checks that the insn is a memory access
    if (ND_ACCESS_READ != (insn->MemoryAccess & ND_ACCESS_READ)) {
        fprintf(stderr, "bddisasm: Access is not read\n");
        return false;
    }

    char insn_str[ND_MIN_BUF_SIZE];
    switch (insn->Instruction)
    {
        case ND_INS_MOVZX:  // fall-through
        case ND_INS_MOVSXD: // fall-through
        case ND_INS_MOV:
        {
            *size = insn->Operands[0].Size;
            break;
        }
        default:
            // display instruction
            NdToText(insn, 0, sizeof(insn_str), insn_str);
            fprintf(stderr, "Unimplemented insn: %s\n", insn_str);
            return false;
    }

    return true;
}

event_response_t cb_on_rw_access(vmi_instance_t vmi, vmi_event_t *event)
{
    (void)vmi;
    cb_data_t *cb_data = (cb_data_t*)event->data;
    event_response_t rsp = VMI_EVENT_RESPONSE_NONE;

    char str_access[4] = {'_', '_', '_', '\0'};
    if (event->mem_event.out_access & VMI_MEMACCESS_R) str_access[0] = 'R';
    if (event->mem_event.out_access & VMI_MEMACCESS_W) str_access[1] = 'W';
    if (event->mem_event.out_access & VMI_MEMACCESS_X) str_access[2] = 'X';

    printf("%s: %s access at 0x%"PRIx64", on frame 0x%"PRIx64", at offset 0x%"PRIx64", generated by insn at 0x%"PRIx64"\n",
           __func__, str_access, event->mem_event.gla, event->mem_event.gfn, event->mem_event.offset, event->x86_regs->rip);

    if (!(event->mem_event.out_access & VMI_MEMACCESS_R)) {
        // not a read event. skip.
        return rsp;
    }

    // read a buffer of an x86 insn max size at RIP (15 Bytes)
    uint8_t insn_buffer[MAX_SIZE_X86_INSN] = {0};
    size_t bytes_read = 0;
    if (VMI_FAILURE == vmi_read_va(vmi, event->x86_regs->rip, 0, MAX_SIZE_X86_INSN, insn_buffer, &bytes_read)) {
        fprintf(stderr, "Failed to read buffer at RIP\n");
        return rsp;
    }

    // check bytes_read
    if (bytes_read != MAX_SIZE_X86_INSN) {
        fprintf(stderr, "Failed to read enough bytes at RIP\n");
        return rsp;
    }

    // disassemble next instruction with libbdisasm
    uint8_t defcode = ND_CODE_32;
    uint8_t defdata = ND_DATA_32;
    if (cb_data->is64) {
        defcode = ND_CODE_64;
        defdata = ND_DATA_64;
    }

    INSTRUX rip_insn;
    NDSTATUS status = NdDecodeEx(&rip_insn, insn_buffer, sizeof(insn_buffer), defcode, defdata);
    if (!ND_SUCCESS(status)) {
        fprintf(stderr, "Failed to decode instruction with libbdisasm: %x\n", status);
        return rsp;
    }

    // determine memory access size
    size_t access_size = 0;
    if (!mem_access_size_from_insn(&rip_insn, &access_size)) {
        return rsp;
    }
    printf("Read access size: %ld\n", access_size);

    if (event->x86_regs->rip == cb_data->ntload_driver_entry_addr) {
        printf("READ attempt on NtLoadDriver SSDT entry !\n");
        // set data to emulate
        event->emul_read = &cb_data->emul_read;
        // set response to emulate read data
        rsp |= VMI_EVENT_RESPONSE_SET_EMUL_READ_DATA;
    }

    return rsp;
}

int main (int argc, char **argv)
{
    vmi_instance_t vmi = {0};
    struct sigaction act = {0};
    vmi_init_data_t *init_data = NULL;
    bool is_corrupted = false;
    addr_t ntload_driver_entry_addr = 0;
    int retcode = 1;
    // whether our Windows guest is 64 bits
    bool is64 = false;

    act.sa_handler = close_handler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGHUP,  &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT,  &act, NULL);
    sigaction(SIGALRM, &act, NULL);

    char *name = NULL;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <name of VM> [<socket>]\n", argv[0]);
        return retcode;
    }

    // Arg 1 is the VM name.
    name = argv[1];

    if (argc == 3) {
        char *path = argv[2];

        // fill init_data
        init_data = malloc(sizeof(vmi_init_data_t) + sizeof(vmi_init_data_entry_t));
        init_data->count = 1;
        init_data->entry[0].type = VMI_INIT_DATA_KVMI_SOCKET;
        init_data->entry[0].data = strdup(path);
    }

    // Initialize the libvmi library.
    if (VMI_FAILURE ==
            vmi_init_complete(&vmi, name, VMI_INIT_DOMAINNAME | VMI_INIT_EVENTS, init_data,
                              VMI_CONFIG_GLOBAL_FILE_ENTRY, NULL, NULL)) {
        printf("Failed to init LibVMI library.\n");
        goto error_exit;
    }

    printf("LibVMI init succeeded!\n");
    uint8_t addr_width = vmi_get_address_width(vmi);
    if (addr_width == sizeof(uint64_t))
        is64 = true;

    // pause
    printf("Pausing VM\n");
    if (VMI_FAILURE == vmi_pause_vm(vmi)) {
        fprintf(stderr, "Failed to pause vm\n");
        goto error_exit;
    }

    // read nt!KeServiceDescriptorTable
    addr_t ke_sd_table_addr = 0;
    if (VMI_FAILURE == vmi_translate_ksym2v(vmi, "KeServiceDescriptorTable", &ke_sd_table_addr)) {
        fprintf(stderr, "Failed to translate KeServiceDescriptorTable symbol\n");
        goto error_exit;
    }
    printf("nt!KeServiceDescriptorTable: 0x%" PRIx64 "\n", ke_sd_table_addr);

    // read nt!KiServiceTable
    addr_t ki_sv_table_addr = 0;
    if (VMI_FAILURE == vmi_translate_ksym2v(vmi, "KiServiceTable", &ki_sv_table_addr)) {
        fprintf(stderr, "Failed to translate KiServiceTable symbol\n");
        goto error_exit;
    }
    printf("nt!KiServiceTable: 0x%" PRIx64 "\n", ki_sv_table_addr);

    // read nt!NtLoadDriver
    addr_t ntload_driver_addr = 0;
    if (VMI_FAILURE == vmi_translate_ksym2v(vmi, "NtLoadDriver", &ntload_driver_addr)) {
        fprintf(stderr, "Failed to translate NtAddDriverEntry symbol\n");
        goto error_exit;
    }
    printf("nt!NtLoadDriver: 0x%" PRIx64 "\n", ntload_driver_addr);

    /*
     * Table's structure looks like the following
     * (source: https://m0uk4.gitbook.io/notebooks/mouka/windowsinternal/ssdt-hook)

        struct SSDTStruct
        {
            LONG* pServiceTable;
            PVOID pCounterTable;
        #ifdef _WIN64
            ULONGLONG NumberOfServices;
        #else
            ULONG NumberOfServices;
        #endif
            PCHAR pArgumentTable;
        };
    */


    //  read NumberOfServices
    addr_t nb_services_addr = ke_sd_table_addr + (addr_width * 2);
    addr_t nb_services = 0;
    if (VMI_FAILURE == vmi_read_addr_va(vmi, nb_services_addr, 0, &nb_services)) {
        fprintf(stderr, "Failed to read SSDT.NumberOfServices field\n");
        goto error_exit;
    }
    printf("SSDT.NumberOfServices: 0x%" PRIx64 "\n", nb_services);

    // find NtLoadDriverEntry index in SSDT
    int ntload_service_table_index = -1;
    uint32_t ntload_service_table_val = 0;
    for (int i = 0; i < (int)nb_services; i++) {
        addr_t ki_service_entry_addr = ki_sv_table_addr + (sizeof(uint32_t) * i);
        uint32_t ki_service_entry_val = 0;
        if (VMI_FAILURE == vmi_read_32_va(vmi, ki_service_entry_addr, 0, &ki_service_entry_val)) {
            fprintf(stderr, "Failed to read syscall address\n");
            goto error_exit;
        }
        // 32 bits: KiServiceTable entries are absolute addresses to the syscall handlers
        addr_t syscall_addr = ki_service_entry_val;
        // 64 bits: KiServiceTable entries are offsets
        //      https://www.ired.team/miscellaneous-reversing-forensics/windows-kernel-internals/glimpse-into-ssdt-in-windows-x64-kernel
        //      RoutineAbsoluteAddress = KiServiceTableAddress + ( routineOffset >>> 4 )
        if (is64) {
            syscall_addr = ki_sv_table_addr + (ki_service_entry_val >> 4);
        }

        // Debug
        // printf("Syscall[%d]: 0x%" PRIx64 "\n", i, syscall_addr);
        // find NtLoadDriver
        if (syscall_addr == ntload_driver_addr) {
            printf("Found NtLoadDriver SSDT entry: %d\n", i);
            ntload_service_table_index = i;
            ntload_service_table_val = ki_service_entry_val;
            break;
        }
    }
    if (ntload_service_table_index == -1) {
        fprintf(stderr, "Failed to find NtLoadDriver SSDT entry\n");
        goto error_exit;
    }

    // corrupting pointer
    printf("Corrupting NtLoadDriver SSDT entry\n");
    ntload_driver_entry_addr = ki_sv_table_addr + (sizeof(uint32_t) * ntload_service_table_index);
    uint32_t corrupted_value = 0;
    if (VMI_FAILURE == vmi_write_32_va(vmi, ntload_driver_entry_addr, 0, &corrupted_value)) {
        fprintf(stderr, "Failed to corrupt NtLoadDriver SSDT entry\n");
        goto error_exit;
    }
    is_corrupted = true;

    // flush page cache after write
    vmi_pagecache_flush(vmi);

    // reread NtLoadDriver SSDT entry
    if (VMI_FAILURE == vmi_read_32_va(vmi, ntload_driver_entry_addr, 0, &corrupted_value)) {
        fprintf(stderr, "Failed to read NtLoadDriver SSDT entry\n");
        goto error_exit;
    }
    printf("New NtLoadDriver SSDT entry value: 0x%" PRIx32 "\n", corrupted_value);

    // protect corrupted SSDT entry using memory access event
    //   get dtb
    uint64_t cr3;
    if (VMI_FAILURE == vmi_get_vcpureg(vmi, &cr3, CR3, 0)) {
        fprintf(stderr, "Failed to get current CR3\n");
        goto error_exit;
    }
    uint64_t dtb = cr3 & ~(0xfff);
    // get paddr
    uint64_t syscall_entry_paddr;
    if (VMI_FAILURE == vmi_pagetable_lookup(vmi, dtb, ntload_driver_entry_addr, &syscall_entry_paddr)) {
        fprintf(stderr, "Failed to find current paddr\n");
        goto error_exit;
    }
    // get Guest Frame Number (gfn)
    uint64_t syscall_entry_gfn = syscall_entry_paddr >> 12;
    vmi_event_t read_event = {0};
    SETUP_MEM_EVENT(&read_event, syscall_entry_gfn, VMI_MEMACCESS_RW, cb_on_rw_access, 0);
    // add cb_data
    cb_data_t cb_data = {0};
    cb_data.is64 = is64;
    cb_data.ntload_driver_entry_addr = ntload_driver_entry_addr;
    cb_data.emul_read.dont_free = 1;
    cb_data.emul_read.size = sizeof(ntload_driver_addr);
    memcpy(&cb_data.emul_read.data, &ntload_driver_addr, cb_data.emul_read.size);
    // set event callback data
    read_event.data = (void*)&cb_data;

    printf("Registering read event on GFN 0x%" PRIx64 "\n", syscall_entry_gfn);
    if (VMI_FAILURE == vmi_register_event(vmi, &read_event)) {
        fprintf(stderr, "Failed to register event\n");
        goto error_exit;
    }

    // resume
    printf("Resuming VM\n");
    if (VMI_FAILURE == vmi_resume_vm(vmi)) {
        fprintf(stderr, "Failed to continue VM\n");
        goto error_exit;
    }
    printf("Waiting for events...\n");
    while (!interrupted) {
        if (VMI_FAILURE == vmi_events_listen(vmi,500)) {
            fprintf(stderr, "Failed to listen on VMI events\n");
            goto error_exit;
        }
    }
    printf("Finished with test.\n");

    retcode = 0;
error_exit:
    if (is_corrupted) {
        printf("Restoring NtLoadDriver SSDT entry\n");
        // restore SSDT entry
        if (VMI_FAILURE == vmi_write_32_va(vmi, ntload_driver_entry_addr, 0, &ntload_service_table_val)) {
            fprintf(stderr, "Failed to restore SSDT entry\n");
        }
    }
    vmi_resume_vm(vmi);
    // cleanup any memory associated with the libvmi instance
    vmi_destroy(vmi);

    if (init_data) {
        free(init_data->entry[0].data);
        free(init_data);
    }

    return retcode;
}
