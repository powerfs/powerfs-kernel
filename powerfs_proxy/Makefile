# PowerFS 用户态代理程序 Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -static -lpthread

TARGET = powerfs_proxy
SRC = powerfs_proxy.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "编译完成: $(TARGET) (静态链接)"

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

run: $(TARGET)
	@echo "启动 PowerFS 用户态代理..."
	./$(TARGET)

test: $(TARGET)
	@echo "测试代理程序..."
	@echo "创建测试设备节点..."
	mkdir -p /dev
	@echo "请先加载 powerfs 内核模块: insmod ../powerfs_mod/powerfs.ko"
	@echo "然后运行: ./$(TARGET)"
