#include "dramSkiplist.h"
#include "pmemSkiplist.h"
class TandemIndex {
    public:
        TandemIndex() {
            //head is the top layer first node
            mainIndex = new DramSkiplist();
            shadowIndex = new PmemSkiplist();
        }

        ~TandemIndex() {
        }
    DramSkiplist *mainIndex;
    PmemSkiplist *shadowIndex;
};