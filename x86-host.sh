if [ $# -lt 1 ]
then
  echo "usage: $0 device fs_image"
  echo "supported devices: byt, cht, bsw, hsw, bdw, bxt, apl, skl, kbl, cfl, cnl, cml, icl, tgl, ehl"
  exit
fi

case $1 in
*byt)
 ADSP="-device driver=adsp-byt"
 HDA=""
  ;;
*cht)
 ADSP="-device driver=adsp-cht"
 HDA=""
  ;;
*bsw)
 ADSP="-device driver=adsp-cht"
 HDA=""
  ;;
*hsw)
 ADSP="-device driver=adsp-hsw"
 HDA=""
  ;;
*bdw)
 ADSP="-device driver=adsp-bdw"
 HDA=""
  ;;
*bxt)
 HDA=" -device bxt-intel-hda,id=sound0,bus=pci.0,addr=0xe"
  ;;
*apl)
 HDA=" -device apl-intel-hda,id=sound0,bus=pci.0,addr=0xe,debug=1"
  ;;
*skl)
 HDA=" -device skl-intel-hda,id=sound0,bus=pci.0,addr=0xe"
  ;;
*kbl)
 HDA=" -device kbl-intel-hda,id=sound0,bus=pci.0,addr=0xe"
  ;;
*cml_lp)
 HDA=" -device cml-lp-intel-hda,id=sound0,bus=pci.0,addr=0xe"
  ;;
*cml)
 HDA=" -device cml-hp-intel-hda,id=sound0,bus=pci.0,addr=0xe"
  ;;
*cfl)
 HDA=" -device cfl-intel-hda,id=sound0,bus=pci.0,addr=0xe"
  ;;
*cnl)
 HDA=" -device cnl-intel-hda,id=sound0,bus=pci.0,addr=0xe"
  ;;
*icl)
 HDA=" -device icl-intel-hda,id=sound0,bus=pci.0,addr=0xe"
  ;;
*tgl)
 HDA=" -device tgl-intel-hda,id=sound0,bus=pci.0,addr=0xe"
  ;;
*ehl)
 HDA=" -device ehl-intel-hda,id=sound0,bus=pci.0,addr=0xe"
  ;;
*)
  echo "usage: $0 device"
  echo "supported devices: byt, cht, bsw, hsw, bdw, bxt, apl, skl, kbl, cfl, cml, icl, tgl, ehl"
  ./xtensa-softmmu/qemu-system-xtensa -machine help
  exit
  ;;
esac

./x86_64-softmmu/qemu-system-x86_64 \
	-enable-kvm -machine pc,accel=kvm -smp 2,sockets=2,cores=1,threads=1 \
	-device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:02 \
	-netdev user,id=net0,hostfwd=tcp::5555-:22 \
	-drive file=/home/lrg/work/poky/build/tmp/deploy/images/qemux86-64/core-image-minimal-qemux86-64.ext4,if=virtio,format=raw \
	-show-cursor -usb -device usb-tablet \
	-object rng-random,filename=/dev/urandom,id=rng0 \
	-device virtio-rng-pci,rng=rng0 \
	-cpu core2duo -m 256 \
	$HDA \
	-serial mon:vc -serial null \
	-kernel /home/lrg/work/linux/arch/x86_64/boot/bzImage -append 'root=/dev/vda rw highres=off  mem=256M ip=192.168.7.2::192.168.7.1:255.255.255.0 uvesafb.mode_option=640x480-32 oprofile.timer=1 uvesafb.task_timeout=-1 '

exit

./x86_64-softmmu/qemu-system-x86_64 \
	-enable-kvm -machine pc,accel=kvm -smp 2,sockets=2,cores=1,threads=1 \
	-device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:02 \
	-netdev user,id=net0,hostfwd=tcp::5555-:22 \
	-drive file=/home/lrg/work/poky/build/tmp/deploy/images/qemux86-64/core-image-minimal-qemux86-64.ext4,if=virtio,format=raw \
	-vga vmware -show-cursor -usb -device usb-tablet \
	-object rng-random,filename=/dev/urandom,id=rng0 \
	-device virtio-rng-pci,rng=rng0 \
	-cpu core2duo -m 256 \
	$HDA \
	-serial mon:vc -serial null \
	-kernel /home/lrg/work/linux/arch/x86_64/boot/bzImage -append 'root=/dev/vda rw highres=off  mem=256M ip=192.168.7.2::192.168.7.1:255.255.255.0 vga=0 uvesafb.mode_option=640x480-32 oprofile.timer=1 uvesafb.task_timeout=-1 '

exit

# start the x86 host use -gdb flag to enable GDB
./x86_64-softmmu/qemu-system-x86_64 -enable-kvm -name ubuntu-intel  \
	-machine pc,accel=kvm,usb=off \
	-cpu SandyBridge -m 2048 \
	-realtime mlock=off \
	-smp 2,sockets=2,cores=1,threads=1 \
	-uuid f5e20908-1854-41b5-b2ca-81b2f007d92d \
	-no-user-config -nodefaults \
	-rtc base=utc,driftfix=slew \
	-global kvm-pit.lost_tick_policy=discard -no-hpet \
	-no-shutdown \
	-global PIIX4_PM.disable_s3=1 \
	-global PIIX4_PM.disable_s4=1 \
	-boot strict=on \
	-device virtio-serial-pci,id=virtio-serial0,bus=pci.0,addr=0x6 \
	-chardev pty,id=charserial0 \
	-device isa-serial,chardev=charserial0,id=serial0 \
	-chardev spicevmc,id=charchannel0,name=vdagent \
	-device virtserialport,bus=virtio-serial0.0,nr=1,chardev=charchannel0,id=channel0,name=com.redhat.spice.0 \
	-spice port=5902,addr=127.0.0.1,disable-ticketing,seamless-migration=on \
	-device qxl-vga,id=video0,ram_size=67108864,vram_size=67108864,vgamem_mb=16,bus=pci.0,addr=0x2 \
	-device virtio-balloon-pci,id=balloon0,bus=pci.0,addr=0x8 \
	-msg timestamp=on \
	-display sdl \
	-netdev user,id=network0,hostfwd=tcp::5555-:22 -device e1000,netdev=network0 \
	-monitor stdio $HDA $ADSP -drive file=../poky/build/tmp/deploy/images/qemux86-64/core-image-minimal-qemux86-64.ext4,if=virtio,format=raw

