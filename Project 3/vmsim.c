#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

int num_frames;

int clock(unsigned int memory[], FILE* f);
int aging(unsigned int memory[], FILE* f, int refresh_rate);
int opt(unsigned int memory[], FILE* f);
int working_set_clock(unsigned int memory[], FILE* f, int tao);
int find_location(unsigned int memory[], int address);
int get_referenced_bit(unsigned int memory[], int location);
int set_referenced_bit(unsigned int memory[], int location);
int unset_referenced_bit(unsigned int memory[], int location);
int is_dirty(unsigned int memory[], int location);
int set_dirty_bit(unsigned int memory[], int location);
int replace(unsigned int memory[], int location, int address);
void print_results(long accesses, long faults, long writes);
void fill_buffer(unsigned int buffer[], FILE *f);
int measure_distance(unsigned int address, unsigned int buffer[], int counter);

int is_dirty(unsigned int memory[], int location){
  return memory[location] & 1;
}

int set_dirty_bit(unsigned int memory[], int location){
  memory[location] |= 1;
  return 0;
}

int unset_dirty_bit(unsigned int memory[], int location){
  memory[location] &= 0xFFFFFFFE;
  return 0;
}

int replace(unsigned int memory[], int location, int address){
  memory[location] = ((address>>12)<<12);
  return 0;
}

int get_referenced_bit(unsigned int memory[], int location){
  return (memory[location]&2)>>1;
}

int set_referenced_bit(unsigned int memory[], int location){
  memory[location] |= 2;
  return 0;
}

int unset_referenced_bit(unsigned int memory[], int location){
  memory[location] &= 0xFFFFFFFD;
  return 0;
}

int find_location(unsigned int memory[], int address){
  int found = -1;
  int i;
  address = address >> 12;
  for(i=0; i<num_frames; i++){
    if(address == memory[i] >> 12){
      found = i;
      break;
    }
  }
  return found;
}

int aging(unsigned int memory[], FILE* f, int refresh_rate){
  unsigned int address;
  char mode;
  int refresh = refresh_rate;
  unsigned char age[num_frames];
  int i;
  int replacement;

  long faults = 0; long accesses = 0; long writes = 0;

  for(i=0;i<num_frames;i++){
    age[i]=0;
  }

  while(1){
    int current = fscanf(f, "%x %c", &address, &mode);
    if (current<2){
      break;
    }

    accesses++;
    //refresh all refernce bits
    if (refresh == 0){
      for (i=0;i<num_frames;i++){
        unset_referenced_bit(memory, i);
      }
      refresh  = refresh_rate;
    }

    // shift the age counter to the right every tick
    for (i=0; i<num_frames; i++){
      age[i] = age[i] >> 1;
      if (get_referenced_bit(memory, i) == 1){
        age[i] |= 128;
      }
    }

    int location = find_location(memory, address);
    if (location == -1){
      faults++;

      if (faults<=num_frames){
        for (i=0;i<num_frames;i++){
          if(memory[i] == 0){
            location = i;
          }
        }
      }
      else {
        int oldest_page = 0;
        for(i=0;i<num_frames;i++){
          if (age[i] == 0){
            location = i;
            break;
          }
          else if (age[i] < oldest_page){
            oldest_page = age[i];
            location = i;
          }
        }
      }
      if (is_dirty(memory, location)){
        printf("page fault - is dirty\n");
        writes++;
      }
      else {
        if (faults < num_frames){
          printf("page fault - no evict\n");
        }
        else {
          printf("page fault - evict clean\n");
        }
      }
      replace(memory, location, address);
    }
    else {
      printf("hit\n");
    }
    refresh--;
    age[location] &= 0;
    set_referenced_bit(memory, location);
    if(mode == 'w' || mode == 'W'){
      set_dirty_bit(memory, location);
    }
    }
  print_results(accesses, faults, writes);
}

int clock(unsigned int memory[], FILE* f){
  unsigned int address;
  char mode;

  long faults = 0; long accesses = 0; long writes = 0;

  int clock_hand = 0;

  while(1){
    int current = fscanf(f, "%x %c", &address, &mode);
    if (current<2){
      break;
    }

    accesses++;

    int location = find_location(memory, address);
    if (location == -1){
      faults++;

      int referenced_bit = get_referenced_bit(memory, clock_hand);
      while(referenced_bit == 1){
        unset_referenced_bit(memory, clock_hand);
        clock_hand++;
        clock_hand%=num_frames;
        referenced_bit = get_referenced_bit(memory, clock_hand);
      }

      if(is_dirty(memory, clock_hand) == 1){
        printf("page fault - is dirty\n");
        writes++;
      }
      else{
        if (faults < num_frames){
          printf("page fault - no evict\n");
        }
        else {
          printf("page fault - evict clean\n");
        }
      }

      replace(memory, clock_hand, address);

      location = clock_hand;
      clock_hand++;
      clock_hand%=num_frames;
    }
    else {
      printf("hit\n");
    }
    if(mode == 'w' || mode == 'W'){
      set_dirty_bit(memory, location);
    }

    set_referenced_bit(memory, location);
  }

  print_results(accesses, faults, writes);
}

void fill_buffer(unsigned int buffer[], FILE *f){
  int i = 0;
  int j;
  char mode;
  unsigned int address;
  while(1){
    int current = fscanf(f, "%x %c", &address, &mode);
    if (current < 2){
      break;
    }
    buffer[i] = ((address>>12)<<12);
    i++;
  }
}

int measure_distance(unsigned int address, unsigned int buffer[], int counter){
  int i;
  int distance = 0;
  for(i = counter; i < 1000000; i++){
    if (buffer[i] == (address>>12)<<12){
      break;
    }
    distance++;
  }
  return distance;
}

int opt(unsigned int memory[], FILE* f){
  unsigned int address;
  char mode;

  long faults = 0; long accesses = 0; long writes = 0;

  unsigned int buffer[1000000];
  int counter = 0;
  int i;

  for (i = 0; i < 1000000; i++){
    fscanf(f, "%x %c", &address, &mode);
    buffer[i] = (address >> 12) << 12;
  }

  fseek(f, 0, SEEK_SET);

  while(1){
    if (fscanf(f, "%x %c", &address, &mode) < 2){
      break;
    }

    accesses++;
    counter++;

    int location = find_location(memory, address);

    if (location == -1){
      faults ++;

      if(faults<num_frames) {
        for(i = 0; i < num_frames; i++){
          if(memory[i] == 0){
            replace(memory, i , address);
            break;
          }
        }
      }
      else {
        int distance = 0;
        int farthest = 0;
        int replacement;

        for (i = 0; i < num_frames; i++){
          distance = measure_distance(memory[i], buffer, counter);
          if(distance > farthest){
            farthest = distance;
            replacement = i;
          }
        }
        if(is_dirty(memory, replacement) == 1){
          printf("page fault - is dirty\n");
          writes++;
        }
        else{
          if (faults < num_frames){
            printf("page fault - no evict\n");
          }
          else {
            printf("page fault - evict clean\n");
          }
        }
        replace(memory, replacement, address);
        location = replacement;
      }
    }
    else {
      printf("hit\n");
    }
    if(mode == 'w' || mode == 'W'){
      set_dirty_bit(memory, location);
    }

  }
  print_results(accesses, faults, writes);
}

int working_set_clock(unsigned int memory[], FILE* f, int tau){
  unsigned int address;
  char mode;
  int tau_list[num_frames];
  long faults = 0; long accesses = 0; long writes = 0;
  int counter = 0;
  int clock_hand = 0;
  int i;

  while(1){
    if (fscanf(f, "%x %c", &address, &mode) < 2){
      break;
    }
    accesses++;
    counter++;
    int evict_page;

    int location = find_location(memory, address);
    if (location == -1){
      faults++;
      if (faults<=num_frames){
        for (i=0;i<num_frames;i++){
          if(memory[i] == 0){
            evict_page = i;
          }
        }
      }
      else{
        int full_loop = 0;
        while(full_loop!=num_frames){
          int referenced_bit = get_referenced_bit(memory, clock_hand);
          if (referenced_bit == 1){
            tau_list[clock_hand] = counter;
            unset_referenced_bit(memory, clock_hand);
          }
          else {
            int time_distance = counter - tau_list[clock_hand];
            if ((is_dirty(memory, clock_hand ) != 1) && (time_distance > tau)){
                evict_page = clock_hand;
                break;
            }
            else if (is_dirty(memory, clock_hand) == 1){
              tau_list[clock_hand] = counter;
              writes++;
              unset_dirty_bit(memory, clock_hand);
            }
          }
          clock_hand++;
          clock_hand%=num_frames;
          full_loop++;
        }
        if (full_loop == num_frames){
          int greatest_distance = 0;
          for (i=0;i<num_frames;i++){
            if(tau_list[i] > greatest_distance){
              greatest_distance = tau_list[i];
              evict_page = i;
            }
          }
        }
      }
    }
    else {

    }
    replace(memory, evict_page, address);
    tau_list[evict_page] = counter;
    set_referenced_bit(memory, evict_page);
    if(mode == 'w' || mode == 'W'){
      set_dirty_bit(memory, evict_page);
    }
  }
  print_results(accesses, faults, writes);
}

void print_results(long accesses, long faults, long writes){
  printf("Number of frames:      %d \n", num_frames);
  printf("Total memory accesses: %ld\n", accesses  );
  printf("Total page faults:     %ld\n", faults    );
  printf("Total writes to disk:  %ld\n", writes    );
}

int main(int argc, char* argv[]) {
  num_frames = -1;
  char algorithm[10];
  FILE *f;
  int refresh_rate = 1;
  int tau;

  num_frames = atoi(argv[2]);
  strcpy(algorithm, argv[4]);

  unsigned int memory[num_frames];
  int i;
  for (i=0; i<num_frames; i++){
    memory[i] = 0;
  }
  if (strcmp(algorithm, "opt") == 0){
    f = fopen(argv[5], "r+");
    opt(memory, f);
  }

  else if (strcmp(algorithm, "clock") == 0){
    f = fopen(argv[5], "r+");
    clock(memory, f);
  }

  else if (strcmp(algorithm, "aging") == 0){
    f = fopen(argv[7], "r+");
    refresh_rate = atoi(argv[6]);
    aging(memory, f, refresh_rate);
  }
  else if(strcmp(algorithm, "work") == 0){
    f = fopen(argv[7], "r+");
    tau = atoi(argv[6]);
    working_set_clock(memory, f, tau);
  }
}
