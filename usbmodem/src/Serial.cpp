#include "Serial.h"

#include <cmath>
#include <cerrno>
#include <cstdio>

#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#include "Log.h"

Serial::Serial() {
	
}

Serial::~Serial() {
	close();
}

void Serial::breakTransfer() {
	if (m_wake_fds[0] != -1)
		while (::write(m_wake_fds[1], "w", 1) < 0 && errno == EINTR);
}

int Serial::close() {
	if (m_fd != -1) {
		::close(m_fd);
		m_fd = -1;
	}
	
	if (m_wake_fds[0] != -1) {
		::close(m_wake_fds[0]);
		::close(m_wake_fds[1]);
		
		m_wake_fds[0] = -1;
		m_wake_fds[1] = -1;
	}
	return 0;
}

int Serial::open(std::string device, int speed) {
	if (pipe2(m_wake_fds, O_CLOEXEC | O_NONBLOCK) < 0) {
		LOGE("pipe2() failed, error = %d\n", errno);
		close();
		return ERR_BROKEN;
	}
	
	speed_t baudrate = getBaudrate(speed);
	if (baudrate == B0) {
		LOGE("%s - invalid speed: %d\n", device.c_str(), speed);
		close();
		return ERR_BROKEN;
	}
	
	m_fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NDELAY | O_NONBLOCK);
	if (m_fd < 0) {
		LOGE("%s - open error: %d\n", device.c_str(), m_fd);
		close();
		return ERR_BROKEN;
	}
	
	if (!isatty(m_fd)) {
		LOGE("%s - is not TTY\n", device.c_str());
		close();
		return ERR_BROKEN;
	}
	
	struct termios config;
	if (tcgetattr(m_fd, &config) != 0) {
		LOGE("%s - can't get termios config\n", device.c_str());
		close();
		return ERR_BROKEN;
	}
	
	cfsetispeed(&config, baudrate);
	cfsetospeed(&config, baudrate);
	cfmakeraw(&config);
	
	if (tcsetattr(m_fd, TCSANOW, &config) != 0) {
		LOGE("%s - can't set termios config\n", device.c_str());
		close();
		return ERR_BROKEN;
	}
	
	return 0;
}

int Serial::readChunk(char *data, int size, int timeout_ms) {
	struct pollfd pfd[2] = {
		{.fd = m_fd, .events = POLLIN},
		{.fd = m_wake_fds[0], .events = POLLIN}
	};
	
	int ret;
	do {
		ret = ::poll(pfd, 2, timeout_ms);
	} while (ret < 0 && errno == EINTR);
	
	if (ret < 0) {
		LOGE("poll error: %d\n", errno);
		return ERR_IO;
	}
	
	if ((pfd[0].revents & (POLLERR | POLLHUP)) || (pfd[1].revents & (POLLERR | POLLHUP))) {
		LOGE("poll error, fd is broken...\n");
		return ERR_BROKEN;
	}
	
	if ((pfd[0].revents & POLLIN)) {
		ret = ::read(m_fd, data, size);
		
		if (ret < 0) {
			LOGE("read error: %d\n", errno);
			return ERR_IO;
		}
		
		return ret;
	}
	
	if ((pfd[1].revents & POLLIN)) {
		char buf[4];
		while (::read(m_wake_fds[0], buf, 4) > 0 || errno == EINTR);
		return ERR_INTR;
	}
	
	return 0;
}

int Serial::writeChunk(const char *data, int size, int timeout_ms) {
	struct pollfd pfd[2] = {
		{.fd = m_fd, .events = POLLOUT},
		{.fd = m_wake_fds[0], .events = POLLIN}
	};
	
	int ret;
	do {
		ret = ::poll(pfd, 2, timeout_ms);
	} while (ret < 0 && errno == EINTR);
	
	if (ret < 0) {
		LOGE("poll error: %d\n", errno);
		return ERR_IO;
	}
	
	if ((pfd[0].revents & (POLLERR | POLLHUP)) || (pfd[1].revents & (POLLERR | POLLHUP))) {
		LOGE("poll error, fd is broken...\n");
		return ERR_BROKEN;
	}
	
	if ((pfd[0].revents & POLLOUT)) {
		ret = ::write(m_fd, data, size);
		
		if (ret < 0) {
			LOGE("write error: %d\n", errno);
			return ERR_IO;
		}
		
		return ret;
	}
	
	if ((pfd[1].revents & POLLIN)) {
		char buf[4];
		while (::read(m_wake_fds[0], buf, 4) > 0 || errno == EINTR);
		return ERR_INTR;
	}
	
	return 0;
}

int Serial::read(char *data, int size, int timeout_ms) {
	int64_t start = getCurrentTimestamp();
	
	int readed = 0;
	int next_timeout = timeout_ms;
	do {
		int ret = readChunk(data + readed, size - readed, next_timeout);
		if (ret < 0)
			return ret;
		
		readed += ret;
		
		next_timeout = timeout_ms - (getCurrentTimestamp() - start);
	} while (readed < size && next_timeout > 0);
	
	return readed;
}

int Serial::write(const char *data, int size, int timeout_ms) {
	int64_t start = getCurrentTimestamp();
	
	int written = 0;
	int next_timeout = timeout_ms;
	do {
		int ret = writeChunk(data + written, size - written, next_timeout);
		if (ret < 0)
			return ret;
		
		written += ret;
		
		next_timeout = timeout_ms - (getCurrentTimestamp() - start);
	} while (written < size && next_timeout > 0);
	
	return written;
}

speed_t Serial::getBaudrate(int speed) {
	switch (speed) {
		case 0:			return B0;
		case 75:		return B75;
		case 110:		return B110;
		case 134:		return B134;
		case 150:		return B150;
		case 200:		return B200;
		case 300:		return B300;
		case 600:		return B600;
		case 1200:		return B1200;
		case 1800:		return B1800;
		case 2400:		return B2400;
		case 4800:		return B4800;
		case 9600:		return B9600;
		case 19200:		return B19200;
		case 38400:		return B38400;
		case 57600:		return B57600;
		case 115200:	return B115200;
		case 230400:	return B230400;
		case 460800:	return B460800;
	}
	return B0;
}
