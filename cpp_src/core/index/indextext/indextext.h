#pragma once

#include "core/ft/config/baseconfig.h"
#include "core/ft/ft_fuzzy/searchers/isearcher.h"
#include "core/ft/ftdsl.h"
#include "core/ft/ftsetcashe.h"
#include "core/ft/fulltextctx.h"
#include "core/ft/idrelset.h"
#include "core/ft/stemmer.h"
#include "core/index/indexunordered.h"
#include "estl/fast_hash_map.h"
#include "estl/flat_str_map.h"
#include "estl/suffix_map.h"

namespace reindexer {
using std::pair;
using std::unique_ptr;

class WordEntry {
public:
	IdRelSet vids_;
};

class PackedWordEntry {
public:
	PackedIdRelSet vids_;
};

template <typename T>
class IndexText : public IndexUnordered<T> {
public:
	IndexText(IndexType _type, const string& _name);
	IndexText(const IndexText<T>& other);

	template <typename U = T>
	IndexText(IndexType _type, const string& _name, const IndexOpts& opts, const PayloadType payloadType, const FieldsSet& fields,
			  typename std::enable_if<is_payload_unord_map_key<U>::value>::type* = 0)
		: IndexUnordered<T>(_type, _name, opts, payloadType, fields) {
		initSearchers();
	}

	SelectKeyResults SelectKey(const KeyValues& keys, CondType condition, SortType stype, Index::ResultType res_type) override final;
	void Commit(const CommitContext& ctx) override final;
	void UpdateSortedIds(const UpdateSortedContext&) override {}
	void Configure(const string& config) override;
	virtual IdSet::Ptr Select(FullTextCtx::Ptr fctx, FtDSLQuery& dsl) = 0;
	virtual void Commit() = 0;

protected:
	struct VDocEntry {
		typename T::mapped_type* keyEntry;
		h_vector<float, 3> wordsCount;
		h_vector<float, 3> mostFreqWordCount;
	};

	h_vector<pair<const string*, int>, 8> getDocFields(const typename T::key_type&, vector<unique_ptr<string>>& bufStrs);

	void initSearchers();

	// Virtual documents, merged. Addresable by VDocIdType
	vector<VDocEntry> vdocs_;

	unordered_map<string, stemmer> stemmers_;

	vector<search_engine::ISeacher::Ptr> searchers_;
	shared_ptr<FtIdSetCache> cache_ft_;

	fast_hash_map<string, int> ftFields_;
	unique_ptr<BaseFTConfig> cfg_;
};

}  // namespace reindexer
