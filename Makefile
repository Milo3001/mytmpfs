obj-m += my_tmpfs.o
my_tmpfs-objs := my_tmpfs_main.o my_tmpfs_super.o my_tmpfs_inode.o my_tmpfs_dir.o my_tmpfs_file.o my_tmpfs_page.o

# 使用当前内核头文件构建内核模块。
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# 删除生成的目标文件和已构建的模块。
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
