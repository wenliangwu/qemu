xtensa_ss = ss.source_set()
xtensa_ss.add(files(
  'mx_pic.c',
  'pic_cpu.c',
  'xtensa_memory.c',
))
xtensa_ss.add(when: 'CONFIG_XTENSA_SIM', if_true: files('sim.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_VIRT', if_true: files('virt.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_XTFPGA', if_true: files('xtfpga.c'))

xtensa_ss.add(when: 'CONFIG_ADSP', if_true: files('adsp-common.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_INTEL_ADSP_BYT', if_true: files('byt.c'))
xtensa_ss.add(when: 'CONFIG_XTENSA_INTEL_ADSP_HSW', if_true: files('hsw.c'))
hw_arch += {'xtensa': xtensa_ss}
