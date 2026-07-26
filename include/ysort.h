#ifndef YSORT_H
#define YSORT_H

/*
    ysort.h - generic stable Y/depth sorter

    Usage:

        Actor temp;
        ysort(actor_list, count, sizeof(Actor), &temp, ActorCompare);

    The compare callback follows qsort convention:
        < 0 : lhs before rhs
        > 0 : lhs after rhs
          0 : equivalent ordering
*/

#include <string.h>

typedef int (*ysort_compare_fn)(
    const void *lhs,
    const void *rhs);

void ysort(
    void *buffer,
    int count,
    int stride,
    void *temp,
    ysort_compare_fn compare);

#endif /* YSORT_H */



#ifdef YSORT_IMPLEMENTATION_INSERT_SORT
#undef YSORT_IMPLEMENTATION_INSERT_SORT
// void *temp is a pointer to single element

void ysort(
    void *buffer,
    int count,
    int stride,
    void *temp,
    ysort_compare_fn compare)
{
    unsigned char *base = (unsigned char *)buffer;

    /*
        Insertion sort:
        
        [sorted][unsorted]
        
        Take one item from unsorted and insert it
        into the correct position in sorted.
    */
    for (int i = 1; i < count; i++) {
        unsigned char *current = base + i * stride;

        memcpy(temp, current, stride);

        int j = i - 1;

        /*
            Only move elements strictly after temp.
            
            The compare == 0 case intentionally does not move.
            This preserves stability and avoids unnecessary swaps.
        */
        while (j >= 0) {
            unsigned char *previous = base + j * stride;

            if (compare(previous, temp) <= 0) {
                break;
            }

            memcpy(previous + stride, previous, stride);
            j--;
        }

        unsigned char *destination = base + (j + 1) * stride;

        /*
            Avoid copying onto itself. It is harmless, but this
            prevents unnecessary memory writes for already sorted data.
        */
        if (destination != current) {
            memcpy(destination, temp, stride);
        }
    }
}

#endif /* YSORT_IMPLEMENTATION_INSERT_SORT */


#ifdef YSORT_IMPLEMENTATION_MERGE_SORT
#undef YSORT_IMPLEMENTATION_MERGE_SORT
// void *temp_buffer is sizeof(element) * count

static void ysort_merge(
    unsigned char *base,
    unsigned char *temp,
    int left,
    int mid,
    int right,
    int stride,
    ysort_compare_fn compare)
{
    int i = left;
    int j = mid;
    int k = left;

    while (i < mid && j < right) {
        unsigned char *a = base + i * stride;
        unsigned char *b = base + j * stride;

        /*
            Take from the left side when equal.
            This is what makes merge sort stable.
        */
        if (compare(a, b) <= 0) {
            memcpy(temp + k * stride, a, stride);
            i++;
        } else {
            memcpy(temp + k * stride, b, stride);
            j++;
        }

        k++;
    }

    while (i < mid) {
        memcpy(temp + k * stride,
               base + i * stride,
               stride);
        i++;
        k++;
    }

    while (j < right) {
        memcpy(temp + k * stride,
               base + j * stride,
               stride);
        j++;
        k++;
    }

    /*
        Copy merged range back.
    */
    for (i = left; i < right; i++) {
        memcpy(base + i * stride,
               temp + i * stride,
               stride);
    }
}


void ysort(
    void *buffer,
    int count,
    int stride,
    void *temp_buffer,
    ysort_compare_fn compare)
{
    if (count <= 1) {
        return;
    }

    unsigned char *base = (unsigned char *)buffer;
    unsigned char *temp = (unsigned char *)temp_buffer;

    /*
        Bottom-up merge sort.
        
        Width:
            1, 2, 4, 8, 16...
    */
    for (int width = 1; width < count; width *= 2) {

        for (int left = 0; left < count; left += width * 2) {

            int mid = left + width;
            int right = left + width * 2;

            if (mid > count) {
                mid = count;
            }

            if (right > count) {
                right = count;
            }

            /*
                Already ordered.
                Avoids copying in common cases.
            */
            if (mid < right &&
                compare(base + (mid - 1) * stride,
                        base + mid * stride) <= 0)
            {
                continue;
            }

            ysort_merge(
                base,
                temp,
                left,
                mid,
                right,
                stride,
                compare);
        }
    }
}

#endif // YSORT_IMPLEMENTATION_MERGE_SORT