#pragma once

static inline __u16 le16_to_cpup(const __le16 *p)
{
	return le16toh(*p);
}

inline static void uinput_set_abs_params(struct uinput_user_dev *dev, unsigned int axis,
				 int min, int max, int fuzz, int flat)
{
	dev->absmin[axis] = min;
	dev->absmax[axis] = max;
	dev->absfuzz[axis] = fuzz;
	dev->absflat[axis] = flat;
}

inline static void uinput_report_event(int uinput_fd, __u16 type, __u16 code, __s32 value)
{
	struct input_event event = {
		.time = { 0 },
		.type = type,
		.code = code,
		.value = value
	};
	
	if (write(uinput_fd, &event, sizeof(event)) < 0) 
		printf("Failed to write to uinput device: %s\n", strerror(errno));
}

inline static void uinput_report_key(int uinput_fd, __u16 code, __s32 value)
{
	uinput_report_event(uinput_fd, EV_KEY, code, !!value);
}

inline static void uinput_report_abs(int uinput_fd, __u16 code, __s32 value)
{
	uinput_report_event(uinput_fd, EV_ABS, code, value);
}

inline static void uinput_sync(int uinput_fd)
{
	uinput_report_event(uinput_fd, EV_SYN, SYN_REPORT, 0);
}