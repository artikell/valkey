/* Do not modify this file, it's automatically generated from utils/generate-unit-test-header.py */
typedef int unitTestProc(int argc, char **argv, int flags);

typedef struct unitTest {
    char *name;
    unitTestProc *proc;
} unitTest;

int test_crc64(int argc, char **argv, int flags);
int test_crc64combine(int argc, char **argv, int flags);
int test_endianconv(int argc, char *argv[], int flags);
int test_intsetValueEncodings(int argc, char **argv, int flags);
int test_intsetBasicAdding(int argc, char **argv, int flags);
int test_intsetLargeNumberRandomAdd(int argc, char **argv, int flags);
int test_intsetUpgradeFromint16Toint32(int argc, char **argv, int flags);
int test_intsetUpgradeFromint16Toint64(int argc, char **argv, int flags);
int test_intsetUpgradeFromint32Toint64(int argc, char **argv, int flags);
int test_intsetStressLookups(int argc, char **argv, int flags);
int test_intsetStressAddDelete(int argc, char **argv, int flags);
int test_kvstoreAdd16Keys(int argc, char **argv, int flags);
int test_kvstoreIteratorRemoveAllKeysNoDeleteEmptyDict(int argc, char **argv, int flags);
int test_kvstoreIteratorRemoveAllKeysDeleteEmptyDict(int argc, char **argv, int flags);
int test_kvstoreDictIteratorRemoveAllKeysNoDeleteEmptyDict(int argc, char **argv, int flags);
int test_kvstoreDictIteratorRemoveAllKeysDeleteEmptyDict(int argc, char **argv, int flags);
int test_quicklistTestMain(int argc, char **argv, int flags);
int test_quicklistCreateList(int argc, char **argv, int flags);
int test_quicklistAddToHeadOfEmptyList(int argc, char **argv, int flags);
int test_quicklistAddToTail5xAtCompress(int argc, char **argv, int flags);
int test_quicklistAddToHead5xAtCompress(int argc, char **argv, int flags);
int test_quicklistAddToTail500xAtCompress(int argc, char **argv, int flags);
int test_quicklistAddToHead500xAtCompress(int argc, char **argv, int flags);
int test_quicklistRotateEmpty(int argc, char **argv, int flags);
int test_quicklistComprassionPlainNode(int argc, char **argv, int flags);
int test_quicklistNextPlainNode(int argc, char **argv, int flags);
int test_quicklistRotatePlainNode(int argc, char **argv, int flags);
int test_quicklistRotateOneValOnce(int argc, char **argv, int flags);
int test_quicklistRotate500Val5000TimesAtCompress(int argc, char **argv, int flags);
int test_quicklistPopEmpty(int argc, char **argv, int flags);
int test_quicklistPop1StringFrom1(int argc, char **argv, int flags);
int test_quicklistPopHead1NumberFrom1(int argc, char **argv, int flags);
int test_quicklistPopHead500From500(int argc, char **argv, int flags);
int test_quicklistPopHead5000From500(int argc, char **argv, int flags);
int test_quicklistIterateForwardOver500List(int argc, char **argv, int flags);
int test_quicklistIterateReverseOver500List(int argc, char **argv, int flags);
int test_quicklistInsertBefore1Element(int argc, char **argv, int flags);
int test_quicklistInsertHeadWhileHeadNodeIsFull(int argc, char **argv, int flags);
int test_quicklistInsertTailWhileTailNodeIsFull(int argc, char **argv, int flags);
int test_quicklistDuplicateEmptyList(int argc, char **argv, int flags);
int test_quicklistDuplicateListOf1Element(int argc, char **argv, int flags);
int test_quicklistDuplicateListOf500(int argc, char **argv, int flags);
int test_quicklistDeleteRangeEmptyList(int argc, char **argv, int flags);
int test_quicklistDeleteRangeOfEntireNodeInListOfOneNode(int argc, char **argv, int flags);
int test_quicklistDeleteRangeOfEntireNodeWithOverflowCounts(int argc, char **argv, int flags);
int test_quicklistDeleteMiddle100Of500List(int argc, char **argv, int flags);
int test_quicklistDeleteLessThanFillButAcrossNodes(int argc, char **argv, int flags);
int test_quicklistDeleteNegative1From500List(int argc, char **argv, int flags);
int test_quicklistDeleteNegative1From500ListWithOverflowCounts(int argc, char **argv, int flags);
int test_quicklistDeleteNegative100From500List(int argc, char **argv, int flags);
int test_quicklistDeletex10Count5From50List(int argc, char **argv, int flags);
int test_quicklistNumbersOnlyListRead(int argc, char **argv, int flags);
int test_quicklistNumbersLargerListRead(int argc, char **argv, int flags);
int test_quicklistNumbersLargerListReadB(int argc, char **argv, int flags);
int test_quicklistInsertOnceInElementsWhileIteratingAtCompress(int argc, char **argv, int flags);
int test_quicklistInsertBefore250NewInMiddleOf500ElementsAtCompress(int argc, char **argv, int flags);
int test_quicklistInsertAfter250NewInMiddleOf500ElementsAtCompress(int argc, char **argv, int flags);
int test_quicklistIndex1x200From500ListAtFill(int argc, char **argv, int flags);
int test_quicklistIndexX1x2From500ListAtFill(int argc, char **argv, int flags);
int test_quicklistIndexX100From500ListAtFill(int argc, char **argv, int flags);
int test_quicklistIndexTooBig1From50ListAtFill(int argc, char **argv, int flags);
int test_quicklistLremTestAtCompress(int argc, char **argv, int flags);
int test_quicklistIterateReverseAndDeleteAtCompress(int argc, char **argv, int flags);
int test_quicklistIteratorAtIndexTestAtCompress(int argc, char **argv, int flags);
int test_quicklistLtrimTestAAtCompress(int argc, char **argv, int flags);
int test_quicklistLtrimTestBAtCompress(int argc, char **argv, int flags);
int test_quicklistLtrimTestCAtCompress(int argc, char **argv, int flags);
int test_quicklistLtrimTestDAtCompress(int argc, char **argv, int flags);
int test_quicklistVerifySpecificCompressionOfInteriorNodes(int argc, char **argv, int flags);
int test_quicklistInsertAfter1Element(int argc, char **argv, int flags);
int test_quicklistBookmarkGetUpdatedToNextItem(int argc, char **argv, int flags);
int test_quicklistBookmarkLimit(int argc, char **argv, int flags);
int test_quicklistCompressAndDecompressQuicklistListpackNode(int argc, char **argv, int flags);
int test_quicklistCompressAndDecomressQuicklistPlainNodeLargeThanUINT32MAX(int argc, char **argv, int flags);
int test_sds(int argc, char **argv, int flags);
int test_sha1(int argc, char **argv, int flags);
int test_string2ll(int argc, char **argv, int flags);
int test_string2l(int argc, char **argv, int flags);
int test_ll2string(int argc, char **argv, int flags);
int test_ld2string(int argc, char **argv, int flags);
int test_fixedpoint_d2string(int argc, char **argv, int flags);
int test_reclaimFilePageCache(int argc, char **argv, int flags);
int test_zmallocInitialUsedMemory(int argc, char **argv, int flags);
int test_zmallocAllocReallocCallocAndFree(int argc, char **argv, int flags);
int test_zmallocAllocZeroByteAndFree(int argc, char **argv, int flags);

unitTest __test_crc64_c[] = {{"test_crc64", test_crc64}, {NULL, NULL}};
unitTest __test_crc64combine_c[] = {{"test_crc64combine", test_crc64combine}, {NULL, NULL}};
unitTest __test_endianconv_c[] = {{"test_endianconv", test_endianconv}, {NULL, NULL}};
unitTest __test_intset_c[] = {{"test_intsetValueEncodings", test_intsetValueEncodings}, {"test_intsetBasicAdding", test_intsetBasicAdding}, {"test_intsetLargeNumberRandomAdd", test_intsetLargeNumberRandomAdd}, {"test_intsetUpgradeFromint16Toint32", test_intsetUpgradeFromint16Toint32}, {"test_intsetUpgradeFromint16Toint64", test_intsetUpgradeFromint16Toint64}, {"test_intsetUpgradeFromint32Toint64", test_intsetUpgradeFromint32Toint64}, {"test_intsetStressLookups", test_intsetStressLookups}, {"test_intsetStressAddDelete", test_intsetStressAddDelete}, {NULL, NULL}};
unitTest __test_kvstore_c[] = {{"test_kvstoreAdd16Keys", test_kvstoreAdd16Keys}, {"test_kvstoreIteratorRemoveAllKeysNoDeleteEmptyDict", test_kvstoreIteratorRemoveAllKeysNoDeleteEmptyDict}, {"test_kvstoreIteratorRemoveAllKeysDeleteEmptyDict", test_kvstoreIteratorRemoveAllKeysDeleteEmptyDict}, {"test_kvstoreDictIteratorRemoveAllKeysNoDeleteEmptyDict", test_kvstoreDictIteratorRemoveAllKeysNoDeleteEmptyDict}, {"test_kvstoreDictIteratorRemoveAllKeysDeleteEmptyDict", test_kvstoreDictIteratorRemoveAllKeysDeleteEmptyDict}, {NULL, NULL}};
unitTest __test_quicklist_c[] = {{"test_quicklistTestMain", test_quicklistTestMain}, {"test_quicklistCreateList", test_quicklistCreateList}, {"test_quicklistAddToHeadOfEmptyList", test_quicklistAddToHeadOfEmptyList}, {"test_quicklistAddToTail5xAtCompress", test_quicklistAddToTail5xAtCompress}, {"test_quicklistAddToHead5xAtCompress", test_quicklistAddToHead5xAtCompress}, {"test_quicklistAddToTail500xAtCompress", test_quicklistAddToTail500xAtCompress}, {"test_quicklistAddToHead500xAtCompress", test_quicklistAddToHead500xAtCompress}, {"test_quicklistRotateEmpty", test_quicklistRotateEmpty}, {"test_quicklistComprassionPlainNode", test_quicklistComprassionPlainNode}, {"test_quicklistNextPlainNode", test_quicklistNextPlainNode}, {"test_quicklistRotatePlainNode", test_quicklistRotatePlainNode}, {"test_quicklistRotateOneValOnce", test_quicklistRotateOneValOnce}, {"test_quicklistRotate500Val5000TimesAtCompress", test_quicklistRotate500Val5000TimesAtCompress}, {"test_quicklistPopEmpty", test_quicklistPopEmpty}, {"test_quicklistPop1StringFrom1", test_quicklistPop1StringFrom1}, {"test_quicklistPopHead1NumberFrom1", test_quicklistPopHead1NumberFrom1}, {"test_quicklistPopHead500From500", test_quicklistPopHead500From500}, {"test_quicklistPopHead5000From500", test_quicklistPopHead5000From500}, {"test_quicklistIterateForwardOver500List", test_quicklistIterateForwardOver500List}, {"test_quicklistIterateReverseOver500List", test_quicklistIterateReverseOver500List}, {"test_quicklistInsertBefore1Element", test_quicklistInsertBefore1Element}, {"test_quicklistInsertHeadWhileHeadNodeIsFull", test_quicklistInsertHeadWhileHeadNodeIsFull}, {"test_quicklistInsertTailWhileTailNodeIsFull", test_quicklistInsertTailWhileTailNodeIsFull}, {"test_quicklistDuplicateEmptyList", test_quicklistDuplicateEmptyList}, {"test_quicklistDuplicateListOf1Element", test_quicklistDuplicateListOf1Element}, {"test_quicklistDuplicateListOf500", test_quicklistDuplicateListOf500}, {"test_quicklistDeleteRangeEmptyList", test_quicklistDeleteRangeEmptyList}, {"test_quicklistDeleteRangeOfEntireNodeInListOfOneNode", test_quicklistDeleteRangeOfEntireNodeInListOfOneNode}, {"test_quicklistDeleteRangeOfEntireNodeWithOverflowCounts", test_quicklistDeleteRangeOfEntireNodeWithOverflowCounts}, {"test_quicklistDeleteMiddle100Of500List", test_quicklistDeleteMiddle100Of500List}, {"test_quicklistDeleteLessThanFillButAcrossNodes", test_quicklistDeleteLessThanFillButAcrossNodes}, {"test_quicklistDeleteNegative1From500List", test_quicklistDeleteNegative1From500List}, {"test_quicklistDeleteNegative1From500ListWithOverflowCounts", test_quicklistDeleteNegative1From500ListWithOverflowCounts}, {"test_quicklistDeleteNegative100From500List", test_quicklistDeleteNegative100From500List}, {"test_quicklistDeletex10Count5From50List", test_quicklistDeletex10Count5From50List}, {"test_quicklistNumbersOnlyListRead", test_quicklistNumbersOnlyListRead}, {"test_quicklistNumbersLargerListRead", test_quicklistNumbersLargerListRead}, {"test_quicklistNumbersLargerListReadB", test_quicklistNumbersLargerListReadB}, {"test_quicklistInsertOnceInElementsWhileIteratingAtCompress", test_quicklistInsertOnceInElementsWhileIteratingAtCompress}, {"test_quicklistInsertBefore250NewInMiddleOf500ElementsAtCompress", test_quicklistInsertBefore250NewInMiddleOf500ElementsAtCompress}, {"test_quicklistInsertAfter250NewInMiddleOf500ElementsAtCompress", test_quicklistInsertAfter250NewInMiddleOf500ElementsAtCompress}, {"test_quicklistIndex1x200From500ListAtFill", test_quicklistIndex1x200From500ListAtFill}, {"test_quicklistIndexX1x2From500ListAtFill", test_quicklistIndexX1x2From500ListAtFill}, {"test_quicklistIndexX100From500ListAtFill", test_quicklistIndexX100From500ListAtFill}, {"test_quicklistIndexTooBig1From50ListAtFill", test_quicklistIndexTooBig1From50ListAtFill}, {"test_quicklistLremTestAtCompress", test_quicklistLremTestAtCompress}, {"test_quicklistIterateReverseAndDeleteAtCompress", test_quicklistIterateReverseAndDeleteAtCompress}, {"test_quicklistIteratorAtIndexTestAtCompress", test_quicklistIteratorAtIndexTestAtCompress}, {"test_quicklistLtrimTestAAtCompress", test_quicklistLtrimTestAAtCompress}, {"test_quicklistLtrimTestBAtCompress", test_quicklistLtrimTestBAtCompress}, {"test_quicklistLtrimTestCAtCompress", test_quicklistLtrimTestCAtCompress}, {"test_quicklistLtrimTestDAtCompress", test_quicklistLtrimTestDAtCompress}, {"test_quicklistVerifySpecificCompressionOfInteriorNodes", test_quicklistVerifySpecificCompressionOfInteriorNodes}, {"test_quicklistInsertAfter1Element", test_quicklistInsertAfter1Element}, {"test_quicklistBookmarkGetUpdatedToNextItem", test_quicklistBookmarkGetUpdatedToNextItem}, {"test_quicklistBookmarkLimit", test_quicklistBookmarkLimit}, {"test_quicklistCompressAndDecompressQuicklistListpackNode", test_quicklistCompressAndDecompressQuicklistListpackNode}, {"test_quicklistCompressAndDecomressQuicklistPlainNodeLargeThanUINT32MAX", test_quicklistCompressAndDecomressQuicklistPlainNodeLargeThanUINT32MAX}, {NULL, NULL}};
unitTest __test_sds_c[] = {{"test_sds", test_sds}, {NULL, NULL}};
unitTest __test_sha1_c[] = {{"test_sha1", test_sha1}, {NULL, NULL}};
unitTest __test_util_c[] = {{"test_string2ll", test_string2ll}, {"test_string2l", test_string2l}, {"test_ll2string", test_ll2string}, {"test_ld2string", test_ld2string}, {"test_fixedpoint_d2string", test_fixedpoint_d2string}, {"test_reclaimFilePageCache", test_reclaimFilePageCache}, {NULL, NULL}};
unitTest __test_zmalloc_c[] = {{"test_zmallocInitialUsedMemory", test_zmallocInitialUsedMemory}, {"test_zmallocAllocReallocCallocAndFree", test_zmallocAllocReallocCallocAndFree}, {"test_zmallocAllocZeroByteAndFree", test_zmallocAllocZeroByteAndFree}, {NULL, NULL}};

struct unitTestSuite {
    char *filename;
    unitTest *tests;
    unitTestProc *testMain;
} unitTestSuite[] = {
    {"test_crc64.c", __test_crc64_c},
    {"test_crc64combine.c", __test_crc64combine_c},
    {"test_endianconv.c", __test_endianconv_c},
    {"test_intset.c", __test_intset_c},
    {"test_kvstore.c", __test_kvstore_c},
    {"test_quicklist.c", __test_quicklist_c},
    {"test_sds.c", __test_sds_c},
    {"test_sha1.c", __test_sha1_c},
    {"test_util.c", __test_util_c},
    {"test_zmalloc.c", __test_zmalloc_c},
};
