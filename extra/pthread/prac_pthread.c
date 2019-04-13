#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NUM_THREADS 4
#define MAX_NUM 40000000

int arr[MAX_NUM];
int temp[MAX_NUM];

int check_sorted(int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (arr[i] != i)
            return 0;
    return 1;
}

// Implement your solution here
void merge(int left, int mid, int right){
    
    int i = left;
    int j = mid+1;
    int k = left;
    int x;
    
    while(i<=mid && j<=right){
        if(arr[i]<=arr[j])
            temp[k++] = arr[i++];
        else
            temp[k++] = arr[j++];
    }

    if(i>mid)
        for(x = j; x<=right; x++)   temp[k++] = arr[x];

    else
        for(x = i; x<=mid; x++)     temp[k++] = arr[x];
    
    for(int x = left; x<=right; x++)    arr[x] = temp[x];

}
void merge_sort(int left, int right){
    
    int mid;

    if(left<right){
        mid = (left+right)/2;
        merge_sort(left, mid);
        merge_sort(mid+1, right);
        merge(left, mid, right);
    }
}
void *do_work(void* arg){
    
    int start = (int)arg * MAX_NUM/4;
    merge_sort(start, start+MAX_NUM/4-1);
    pthread_exit(NULL);
}

///////////////////////////////

int main(void)
{
    srand((unsigned int)time(NULL));
    const int n = MAX_NUM;
    int i;
    pthread_t *thd;

    for (i = 0; i < n; i++)
        arr[i] = i;
    for (i = n - 1; i >= 1; i--)
    {
        int j = rand() % (i + 1);
        int t = arr[i];
        arr[i] = arr[j];
        arr[j] = t;
    }

    printf("Sorting %d elements...\n", n);

    // Create threads and execute.
    thd = malloc(sizeof(pthread_t)*4);

    for(i=0;i < 4; i++) {
		pthread_create(&thd[i],NULL,do_work,(void *)i);
    }
    
    for(i=0;i< 4; i++) {
		pthread_join(thd[i],NULL);
	}

    merge_sort(0, n-1);
    //////////////////////////////

    if (!check_sorted(n))
    {
        printf("Sort failed!\n");
        return 0;
    }

    printf("Ok %d elements sorted\n", n);
    return 0;
}
