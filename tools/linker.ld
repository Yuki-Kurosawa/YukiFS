ENTRY(_start)

SECTIONS
{
    . = 0x400000; /* Set the starting address of the program */
    
    /* All read-only data sections */
    . = 0x4000b0;
    .rodata : {
        *(.rodata*) 
        *(.string*)
    }

    /* All code sections */
    . = 0x4000f4;
    .text : {
        *(.text*)
    }

    /* All initialized data sections */
    /*.data : {
        *(.data*) 
    }*/

    /* All uninitialized data sections */
    /*.bss : {
        *(.bss*) 
    }*/

    /DISCARD/ : {
        * (*) /* Discard any other sections */
    }
}