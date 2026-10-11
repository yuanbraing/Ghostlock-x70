.section .rodata
.balign 16

.global embedded_su_start
.type embedded_su_start, %object
embedded_su_start:
.incbin "assets/ksud"

.global embedded_su_end
.type embedded_su_end, %object
embedded_su_end:
.byte 0

.global embedded_wallpaper_start
.global embedded_wallpaper_end
.balign 16
embedded_wallpaper_start:
.byte 0
embedded_wallpaper_end:
.balign 16
