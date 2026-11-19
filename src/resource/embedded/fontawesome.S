.section .rodata,"a",%progbits
.balign 4
.global g_fontawesome_data
.type g_fontawesome_data, %object
g_fontawesome_data:
.incbin "fontawesome-webfont.ttf"
g_fontawesome_data_end:
.size g_fontawesome_data, . - g_fontawesome_data

.balign 4
.global g_fontawesome_size
.type g_fontawesome_size, %object
g_fontawesome_size:
.int g_fontawesome_data_end - g_fontawesome_data
.size g_fontawesome_size, 4

.section .note.GNU-stack,"",%progbits

