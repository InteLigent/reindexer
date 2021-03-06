#include "resultserializer.h"
#include "core/cjson/tagsmatcher.h"
#include "core/query/queryresults.h"

namespace reindexer {

ResultSerializer::ResultSerializer(bool allowInBuf, const ResultFetchOpts& opts) : WrSerializer(allowInBuf), opts_(opts) {}

void ResultSerializer::putQueryParams(const QueryResults* results) {
	// Pointer to query results
	if (opts_.flags & kResultsWithPtrs) {
		PutUInt64(uintptr_t(results));
	} else {
		PutUInt64(0);
	}

	// Total
	PutVarUint(results->totalCount);
	// Count of returned items by query
	PutVarUint(results->size());
	// Count of serialized items
	PutVarUint(opts_.fetchLimit);

	PutVarUint(results->haveProcent);

	// Count of namespaces
	PutVarUint(results->ctxs.size());

	if (opts_.flags & kResultsWithPayloadTypes) {
		assert(opts_.ptVersions);
		int cnt = 0;
		for (int i = 0; i < results->getMergedNSCount(); i++) {
			const TagsMatcher& tm = results->getTagsMatcher(i);
			if (int32_t(tm.version() ^ tm.cacheToken()) != opts_.ptVersions[i]) cnt++;
		}
		PutVarUint(cnt);
		for (int i = 0; i < results->getMergedNSCount(); i++) {
			const TagsMatcher& tm = results->getTagsMatcher(i);
			if (int32_t(tm.version() ^ tm.cacheToken()) != opts_.ptVersions[i]) {
				PutVarUint(i);
				putPayloadType(results, i);
			}
		}
	} else {
		PutVarUint(0);
	}

	putAggregationParams(results);
}

void ResultSerializer::putAggregationParams(const QueryResults* results) {
	PutVarUint(results->aggregationResults.size());
	for (auto ar : results->aggregationResults) PutDouble(ar);
}

void ResultSerializer::putItemParams(const QueryResults* result, int idx, bool useOffset) {
	int ridx = idx + (useOffset ? opts_.fetchOffset : 0);

	auto& it = result->at(ridx);

	PutVarUint(it.id);
	PutVarUint(it.version);
	PutVarUint(it.nsid);
	PutVarUint(it.proc);
	int format = (opts_.flags & 0x3);

	if (idx < 63 && !(opts_.fetchDataMask & (1 << idx))) {
		format = kResultsPure;
	}

	PutVarUint(format);

	switch (format) {
		case kResultsWithJson:
			result->GetJSON(ridx, *this);
			break;
		case kResultsWithCJson:
			result->GetCJSON(ridx, *this);
			break;
		case kResultsWithPtrs:
			PutUInt64(uintptr_t(it.value.Ptr()));
			break;
		case kResultsPure:
			break;
		default:
			abort();
	}
}

void ResultSerializer::putPayloadType(const QueryResults* results, int nsid) {
	const PayloadType& t = results->getPayloadType(nsid);
	const TagsMatcher& m = results->getTagsMatcher(nsid);

	// Serialize tags matcher
	PutVarUint(m.cacheToken());
	PutVarUint(m.version());
	m.serialize(*this);

	// Serialize payload type
	PutVarUint(base_key_string::export_hdr_offset());
	PutVarUint(t.NumFields());
	for (int i = 0; i < t.NumFields(); i++) {
		PutVarUint(t.Field(i).Type());
		PutVString(t.Field(i).Name());
		PutVarUint(t.Field(i).Offset());
		PutVarUint(t.Field(i).ElemSizeof());
		PutVarUint(t.Field(i).IsArray());
	}
}

bool ResultSerializer::PutResults(const QueryResults* result) {
	if (opts_.fetchOffset > result->size()) {
		opts_.fetchOffset = result->size();
	}

	if (opts_.fetchOffset + opts_.fetchLimit > result->size()) {
		opts_.fetchLimit = result->size() - opts_.fetchOffset;
	}

	putQueryParams(result);

	for (unsigned i = 0; i < opts_.fetchLimit; i++) {
		// Put Item ID and version
		putItemParams(result, i, true);

		auto& it = result->at(i + opts_.fetchOffset);
		auto jres = result->joined_.find(it.id);
		if (jres == result->joined_.end()) {
			PutVarUint(0);
			continue;
		}
		// Put count of joined subqueires for item ID
		PutVarUint(jres->second.size());
		for (auto& jfres : jres->second) {
			// Put count of returned items from joined namespace
			PutVarUint(jfres.size());
			for (unsigned j = 0; j < jfres.size(); j++) {
				putItemParams(&jfres, j, false);
			}
		}
	}
	return opts_.fetchOffset + opts_.fetchLimit >= result->size();
}

}  // namespace reindexer
