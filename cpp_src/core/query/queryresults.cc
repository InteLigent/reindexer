#include "core/query/queryresults.h"
#include "core/cjson/cjsonencoder.h"
#include "core/cjson/jsonencoder.h"
#include "core/itemimpl.h"
#include "tools/logger.h"

namespace reindexer {

struct QueryResults::Context {
	Context() {}
	Context(PayloadType type, TagsMatcher tagsMatcher, JsonPrintFilter jsonFilter)
		: type_(type), tagsMatcher_(tagsMatcher), jsonFilter_(jsonFilter) {}

	PayloadType type_;
	TagsMatcher tagsMatcher_;
	JsonPrintFilter jsonFilter_;
};

static_assert(sizeof(QueryResults::Context) < QueryResults::kSizeofContext,
			  "QueryResults::kSizeofContext should >=  sizeof(QueryResults::Context)");

QueryResults::QueryResults(std::initializer_list<ItemRef> l) : ItemRefVector(l) {}
QueryResults::QueryResults() = default;
QueryResults::QueryResults(QueryResults &&) = default;
QueryResults &QueryResults::operator=(QueryResults &&obj) noexcept {
	if (this != &obj) {
		Erase(begin(), end());
		ItemRefVector::operator=(static_cast<ItemRefVector &&>(obj));
		joined_ = std::move(obj.joined_);
		aggregationResults = std::move(obj.aggregationResults);
		totalCount = std::move(obj.totalCount);
		haveProcent = std::move(obj.haveProcent);
		ctxs = std::move(obj.ctxs);
	}
	return *this;
}

QueryResults::~QueryResults() {
	for (auto &itemRef : *this) {
		if (!itemRef.value.IsFree()) {
			assert(ctxs.size() > back().nsid);
			Payload(ctxs[itemRef.nsid].type_, itemRef.value).ReleaseStrings();
		}
	}
}

void QueryResults::Erase(iterator start, iterator finish) {
	for (auto it = start; it != finish; it++) {
		auto &itemRef = *it;
		if (!itemRef.value.IsFree()) {
			assert(ctxs.size() > back().nsid);
			Payload(ctxs[itemRef.nsid].type_, itemRef.value).ReleaseStrings();
		}
	}
	ItemRefVector::erase(start, finish);
}

void QueryResults::Add(const ItemRef &i) {
	push_back(i);

	auto &itemRef = back();
	if (!itemRef.value.IsFree()) {
		assert(ctxs.size() > back().nsid);
		Payload(ctxs[itemRef.nsid].type_, itemRef.value).AddRefStrings();
	}
}

void QueryResults::Dump() const {
	string buf;
	for (auto &r : *this) {
		if (&r != &*(*this).begin()) buf += ",";
		buf += std::to_string(r.id);
		auto it = joined_.find(r.id);
		if (it != joined_.end()) {
			buf += "[";
			for (auto &ra : it->second) {
				if (&ra != &*it->second.begin()) buf += ";";
				for (auto &rr : ra) {
					if (&rr != &*ra.begin()) buf += ",";
					buf += std::to_string(rr.id);
				}
			}
			buf += "]";
		}
	}
	logPrintf(LogInfo, "Query returned: [%s]; total=%d", buf.c_str(), this->totalCount);
}

class QueryResults::JsonEncoderDatasourceWithJoins : public IJsonEncoderDatasourceWithJoins {
public:
	JsonEncoderDatasourceWithJoins(const vector<QueryResults> &joined, const ContextsVector &ctxs) : joined_(joined), ctxs_(ctxs) {}
	~JsonEncoderDatasourceWithJoins() {}

	size_t GetJoinedRowsCount() final { return joined_.size(); }
	size_t GetJoinedRowItemsCount(size_t rowId) final {
		const QueryResults &queryRes(joined_[rowId]);
		return queryRes.size();
	}
	ConstPayload GetJoinedItemPayload(size_t rowid, size_t plIndex) final {
		const QueryResults &queryRes(joined_[rowid]);
		const ItemRef &itemRef = queryRes[plIndex];
		const Context &ctx = ctxs_[rowid + 1];
		return ConstPayload(ctx.type_, itemRef.value);
	}
	const string &GetJoinedItemNamespace(size_t rowid) final {
		const Context &ctx = ctxs_[rowid + 1];
		return ctx.type_->Name();
	}

private:
	const vector<QueryResults> &joined_;
	const ContextsVector &ctxs_;
};

void QueryResults::encodeJSON(int idx, WrSerializer &ser) const {
	auto &itemRef = at(idx);
	assert(ctxs.size() > itemRef.nsid);
	auto &ctx = ctxs[itemRef.nsid];

	auto itJoined(joined_.find(itemRef.id));
	bool withJoins((itJoined != joined_.end()) && !itJoined->second.empty());

	ConstPayload pl(ctx.type_, itemRef.value);
	JsonEncoder jsonEncoder(ctx.tagsMatcher_, ctx.jsonFilter_);

	if (withJoins) {
		JsonEncoderDatasourceWithJoins ds(itJoined->second, ctxs);
		jsonEncoder.Encode(&pl, ser, ds);
	} else {
		jsonEncoder.Encode(&pl, ser);
	}
}

void QueryResults::GetJSON(int idx, WrSerializer &ser, bool withHdrLen) const {
	assert(static_cast<size_t>(idx) < size());
	if (withHdrLen) {
		// reserve place for size
		uint32_t saveLen = ser.Len();
		ser.PutUInt32(0);

		encodeJSON(idx, ser);

		// put real json size
		int realSize = ser.Len() - saveLen - sizeof(saveLen);
		memcpy(ser.Buf() + saveLen, &realSize, sizeof(saveLen));
	} else {
		encodeJSON(idx, ser);
	}
}

void QueryResults::GetCJSON(int idx, WrSerializer &ser, bool withHdrLen) const {
	auto &itemRef = at(idx);
	assert(ctxs.size() > itemRef.nsid);
	auto &ctx = ctxs[itemRef.nsid];

	ConstPayload pl(ctx.type_, itemRef.value);
	CJsonEncoder cjsonEncoder(ctx.tagsMatcher_);

	if (withHdrLen) {
		// reserve place for size
		uint32_t saveLen = ser.Len();
		ser.PutUInt32(0);

		cjsonEncoder.Encode(&pl, ser);

		// put real json size
		int realSize = ser.Len() - saveLen - sizeof(saveLen);
		memcpy(ser.Buf() + saveLen, &realSize, sizeof(saveLen));
	} else {
		cjsonEncoder.Encode(&pl, ser);
	}
}

Item QueryResults::GetItem(int idx) const {
	auto &itemRef = at(idx);

	assert(ctxs.size() > itemRef.nsid);
	auto &ctx = ctxs[itemRef.nsid];

	PayloadValue v(itemRef.value);

	auto item = Item(new ItemImpl(ctx.type_, v, ctx.tagsMatcher_));
	item.setID(itemRef.id, itemRef.version);
	return item;
}

void QueryResults::AddItem(Item &item) {
	if (item.GetID() != -1) {
		auto ritem = item.impl_;
		ctxs.push_back(Context(ritem->Type(), ritem->tagsMatcher(), JsonPrintFilter()));
		Add(ItemRef(item.GetID(), item.GetVersion()));
	}
}

const TagsMatcher &QueryResults::getTagsMatcher(int nsid) const {
	assert(nsid < int(ctxs.size()));
	return ctxs[nsid].tagsMatcher_;
}

const PayloadType &QueryResults::getPayloadType(int nsid) const {
	assert(nsid < int(ctxs.size()));
	return ctxs[nsid].type_;
}
TagsMatcher &QueryResults::getTagsMatcher(int nsid) {
	assert(nsid < int(ctxs.size()));
	return ctxs[nsid].tagsMatcher_;
}

PayloadType &QueryResults::getPayloadType(int nsid) {
	assert(nsid < int(ctxs.size()));
	return ctxs[nsid].type_;
}
int QueryResults::getMergedNSCount() const { return ctxs.size(); }

void QueryResults::addNSContext(const PayloadType &type, const TagsMatcher &tagsMatcher, const JsonPrintFilter &jsonFilter) {
	ctxs.push_back(Context(type, tagsMatcher, jsonFilter));
}

}  // namespace reindexer
