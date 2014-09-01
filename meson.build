project('xpad2', 'c')

libusb = dependency('libusb-1.0')
pthread = find_library('pthread')

libs = [libusb, pthread]

src = ['xpad360_usb.c', 'xpad360_threadpool.c']

executable('xpad360', src,  \
	dependencies : libs, \
	c_args : ['-std=gnu99'])
