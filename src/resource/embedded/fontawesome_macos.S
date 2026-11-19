.section __DATA, __const
.balign 4
.global _g_fontawesome_data, _g_fontawesome_size

_g_fontawesome_data:
.incbin "fontawesome-webfont.ttf"
_g_fontawesome_data_end:

.balign 4
_g_fontawesome_size:
.int _g_fontawesome_data_end - _g_fontawesome_data


