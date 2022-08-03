.section .data
.balign 4
.arm

.global gamePatchFunc
.type   gamePatchFunc, %function
gamePatchFunc:
    stmfd   sp!, {r0-r12}
    mrs     r0, cpsr
    stmfd   sp!, {r0}
    adr     r0, g_savedGameInstr
    ldr     r1, =0x00100000
    ldr     r2, [r0]
    str     r2, [r1]
    ldr     r2, [r0, #4]
    str     r2, [r1, #4]
    svc     0x92
    svc     0x94

startplugin:
    adr		r0, g_savedGameInstr
	push    {r0}
    ldr     r5, =0x07000100
    blx     r5
    add		sp, sp, #4

exit:
    ldmfd   sp!, {r0}
    msr     cpsr, r0
    ldmfd   sp!, {r0-r12}
    ldr     lr, =0x00100000
    mov     pc, lr

.global g_savedGameInstr
g_savedGameInstr:
    .word 0, 0

.global gameSvcSendSyncRequestHook
.type   gameSvcSendSyncRequestHook, %function
gameSvcSendSyncRequestHook:
    push   {r0, r1, r2, r5, lr}
    mov    r5, r0                   
    mrc    p15, #0, r3, c13, c0, #3 
    ldr    r2, [r3, #0x80]          
    ldr    r0, =0x08040142          
    ldr    r1, =0x08070142          
    cmp    r2, r1                   
    cmpne  r2, r0                   
    bne    sendOriginalRequest      
    str    r2, [r3, #0x84]          
    ldr    r2, =0xE01C0             
    adr    r1, plgldrPortName       
    str    r2, [r3, #0x80]          
    add    r0, sp, #4               
    str    r0, [sp, #-4]!           
    svc    0x2D                     
    ldr    r3, [sp], #4             
    str    r1, [r3]                 
    ldr    r0, [sp, #4]             
    svc    0x32                     
    ldr    r0, [sp, #4]             
    svc    0x23                     
    ldr    r0, =1000000             
    mov    r1, #0                   
    svc    0xA                      
    mov    r0, #0                   

endFunction:
    add    sp, sp, #0xC             
    pop    {r5, pc}                 

sendOriginalRequest:
    mov    r0, r5                   
    svc    0x32                     
    b      endFunction              

plgldrPortName: .asciz "plg:ldr"