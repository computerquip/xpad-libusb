#pragma once

int pool_init(void (*)(unsigned char *, void*));
void pool_queue_work(unsigned char *, void*);
void pool_destroy();