enum {
    NUM_SOCKET = 2,
    NUM_PHYSICAL_CPU_PER_SOCKET = 16,
    SMT_LEVEL = 1,
};

const int OS_CPU_ID[NUM_SOCKET][NUM_PHYSICAL_CPU_PER_SOCKET][SMT_LEVEL] = {
    { /* socket id: 0 */
        { /* physical cpu id: 0 */
          0,     },
        { /* physical cpu id: 1 */
          1,     },
        { /* physical cpu id: 2 */
          2,     },
        { /* physical cpu id: 3 */
          3,     },
        { /* physical cpu id: 4 */
          4,     },
        { /* physical cpu id: 5 */
          5,     },
        { /* physical cpu id: 6 */
          6,     },
        { /* physical cpu id: 7 */
          7,     },
        { /* physical cpu id: 8 */
          8,     },
        { /* physical cpu id: 9 */
          9,     },
        { /* physical cpu id: 10 */
          10,     },
        { /* physical cpu id: 11 */
          11,     },
        { /* physical cpu id: 12 */
          12,     },
        { /* physical cpu id: 13 */
          13,     },
        { /* physical cpu id: 14 */
          14,     },
        { /* physical cpu id: 15 */
          15,     },
    },
    { /* socket id: 1 */
        { /* physical cpu id: 0 */
          16,     },
        { /* physical cpu id: 1 */
          17,     },
        { /* physical cpu id: 2 */
          18,     },
        { /* physical cpu id: 3 */
          19,     },
        { /* physical cpu id: 4 */
          20,     },
        { /* physical cpu id: 5 */
          21,     },
        { /* physical cpu id: 6 */
          22,     },
        { /* physical cpu id: 7 */
          23,     },
        { /* physical cpu id: 8 */
          24,     },
        { /* physical cpu id: 9 */
          25,     },
        { /* physical cpu id: 10 */
          26,     },
        { /* physical cpu id: 11 */
          27,     },
        { /* physical cpu id: 12 */
          28,     },
        { /* physical cpu id: 13 */
          29,     },
        { /* physical cpu id: 14 */
          30,     },
        { /* physical cpu id: 15 */
          31,     },
    },
};
