obj-m += xstat.o

all: xstat.ko

sign: xstat.ko
	perl /usr/src/kernels/$(shell uname -r)/scripts/sign-file sha256 private_key.priv public_key.der xstat.ko

xstat.ko:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
