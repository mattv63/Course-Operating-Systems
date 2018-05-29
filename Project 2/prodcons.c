#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define __NR_cs1550_down 325
#define __NR_cs1550_up 326

struct cs1550_sem{
  int value;
  struct node* head;
  struct node* tail;
};

void down(struct cs1550_sem *sem){
  syscall(__NR_cs1550_down, sem);
}

void up(struct cs1550_sem *sem){
  syscall(__NR_cs1550_down, sem);
}

int main(int argc, char* argv[]){
  int prods; //producers
  int cons; //consumers
  int size;
  int i; //for loops

  prods = strtol(argv[2], NULL, 10);
  cons = strtol(argv[1], NULL, 10);
  size = strtol(argv[3], NULL, 10);

  struct cs1550_sem* individual_memory = (struct cs1550_sem*) mmap(NULL, sizeof(struct cs1550_sem)*3, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, 0, 0);
  int* shared_memory = (int*) mmap(NULL, (size+2)*sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, 0, 0);

  struct cs1550_sem* full;
  struct cs1550_sem* empty;
  struct cs1550_sem* mutex;

  full = individual_memory + 1;
  full->head = NULL;
  full->tail = NULL;
  full->value = 0;

  empty = individual_memory;
  empty->head = NULL;
  empty->tail = NULL;
  empty->value = size;

  mutex = individual_memory + 2;
  mutex->head = NULL;
  mutex->tail = NULL;
  mutex->value = 1;

  int* in = shared_memory;
  int* out = shared_memory + 1;
  int* buffer = shared_memory + 2;

  *in = 0;
  *out = 0;

  for(i = 0; i < prods; i++){
    if (fork() == 0){
      int item;
      while(1){
        down(empty);
        down(mutex);
        item = *in;
        buffer[*in % size] = item;
        printf("Chef %c produced: Pancake%d\n", (i+65), item);
        *in += 1;
        up(mutex);
        up(full);
      }
    }
  }

  for(i = 0; i < cons; i++){
    if (fork() == 0){
      int item;
      while(1){
        down(full);
        down(mutex);
        item = buffer[*out%size];
        printf("Customer %c consumed: Pancake%d\n", (i+65), item);
        *out += 1;
        up(mutex);
        up(empty);
      }
    }
  }

  int x;
  wait(&x);

  return 0;
}
