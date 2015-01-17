/*
 * Database.cpp
 *
 *  Created on: 2014-6-20
 *      Author: liyouhuan
 */
#include "Database.h"


Database::Database(std::string _name){
	this->name = _name;
	std::string store_path = this->name;

	this->signature_binary_file = "signature.binary";
	this->six_tuples_file = "six_tuples";

	std::string kv_store_path = store_path + "/kv_store";
	this->kvstore = new KVstore(kv_store_path);

	std::string vstree_store_path = store_path + "/vs_store";
	this->vstree = new VSTree(vstree_store_path);

	this->encode_mode = Database::STRING_MODE;
	this->sub_num = 0;
	this->pre_num = 0;
	this->literal_num = 0;
	if(Database::fp_debug == NULL)
	{
		fp_debug = fopen("db.log", "w");
		if(fp_debug == NULL){
			cerr << "debug_log open failed" << endl;
		}
	}
}

Database::~Database()
{
    delete this->vstree;
    delete this->kvstore;
	fclose(fp_debug);
}

bool Database::load()
{
    bool flag = (this->vstree)->loadTree();
    if (!flag)
    {
        return false;
    }

	(this->kvstore)->open();
	cout << "finish load" << endl;
	return true;
}
bool Database::unload()
{
    (this->kvstore)->release();
	delete this->vstree;
	return true;
}
string Database::getName()
{
    return this->name;
}

bool Database::query(const string _query, ResultSet& _result_set)
{
	long tv_begin = util::get_cur_time();

	DBparser _parser;
	SPARQLquery _sparql_q(_query);
	_parser.sparqlParser(_query, _sparql_q);

	long tv_parse = util::get_cur_time();
	cout << "after Parsing, used " << (tv_parse - tv_begin) << endl;
	cout << "after Parsing..." << endl << _sparql_q.triple_str() << endl;

	_sparql_q.encodeQuery(this->kvstore);

	cout << "sparqlSTR:\t" << _sparql_q.to_str() << endl;

	long tv_encode = util::get_cur_time();
	cout << "after Encode, used " << (tv_encode - tv_parse) << "ms." << endl;

	_result_set.select_var_num = _sparql_q.getQueryVarNum();

	(this->vstree)->retrieve(_sparql_q);

	long tv_retrieve = util::get_cur_time();
	cout << "after Retrieve, used " << (tv_retrieve - tv_encode) << "ms." << endl;

	this->join(_sparql_q);

	long tv_join = util::get_cur_time();
	cout << "after Join, used " << (tv_join - tv_retrieve) << "ms." << endl;

	this->getFinalResult(_sparql_q, _result_set);

	long tv_final = util::get_cur_time();
	cout << "after finalResult, used " << (tv_final - tv_join) << "ms." << endl;

	cout << "Total time used: " << (tv_final - tv_begin) << "ms." << endl;

	//testing...
	cout << "final result is : " << endl;
	cout << _result_set.to_str() << endl;


	return true;
}
bool Database::insert(const Triple& _triple)
{
	int _sub_id = (this->kvstore)->getIDByEntity(_triple.subject);
	bool _is_new_sub = false;
	/* if sub does not exist */
	if(_sub_id == -1){
		_is_new_sub = true;
		int _new_subid = this->sub_num;
		this->sub_num ++;
		_sub_id = _new_subid;
		(this->kvstore)->setIDByEntity(_triple.subject, _sub_id);
		(this->kvstore)->setEntityByID(_sub_id, _triple.subject);
	}

	int _pre_id = (this->kvstore)->getIDByPredicate(_triple.predicate);
	bool _is_new_pre = false;
	if(_pre_id == -1){
		_is_new_pre = true;
		int _new_pre_id = this->pre_num;
		this->pre_num ++;
		_pre_id = _new_pre_id;
		(this->kvstore)->setIDByPredicate(_triple.predicate, _pre_id);
		(this->kvstore)->setPredicateByID(_pre_id, _triple.predicate);
	}

	/* object is either entity or literal */
	int _obj_id = (this->kvstore)->getIDByEntity(_triple.object);
	if(_obj_id == -1)
	{
		_obj_id = (this->kvstore)->getIDByLiteral(_triple.object);
	}
	bool _is_new_obj = false;
	if(_obj_id == -1)
	{
		_is_new_obj = true;
		int _new_literal_id = Database::LITERAL_FIRST_ID + this->literal_num;
		this->literal_num ++;
		_obj_id = _new_literal_id;
		(this->kvstore)->setIDByLiteral(_triple.object, _obj_id);
		(this->kvstore)->setLiteralByID(_obj_id, _triple.object);
	}

	int _entity_id = _sub_id;

	/* if this is not a new triple, return directly */
	bool _triple_exist = false;
	if(!_is_new_sub &&
	   !_is_new_pre &&
	   !_is_new_obj   )
	{
		_triple_exist = this->exist_triple(_sub_id, _pre_id, _obj_id);
	}
	if(_triple_exist)
	{
		return false;
	}

	/* update sp2o op2s s2po o2ps s2o o2s */
	(this->kvstore)->updateTupleslist_insert(_sub_id, _pre_id, _obj_id);

	EntityBitSet _entity_bitset;
	_entity_bitset.reset();

	this->encodeTriple2EntityBitSet(_entity_bitset, &_triple);

	/* if new entity then insert it, else update it */
	if(_is_new_sub)
	{
		SigEntry _sig(_sub_id, _entity_bitset);
		(this->vstree)->insertEntry(_sig);
	}
	else
	{
		(this->vstree)->updateEntry(_sub_id, _entity_bitset);
	}

	return true;
}
bool Database::remove(const Triple& _triple)
{
	int _sub_id = (this->kvstore)->getIDByEntity(_triple.subject);
	int _pre_id = (this->kvstore)->getIDByPredicate(_triple.predicate);
	int _obj_id = (this->kvstore)->getIDByEntity(_triple.object);
	if(_obj_id == -1){
		_obj_id = (this->kvstore)->getIDByLiteral(_triple.object);
	}

	if(_sub_id == -1 || _pre_id == -1 || _obj_id == -1)
	{
		return false;
	}
	bool _exist_triple = this->exist_triple(_sub_id, _pre_id, _obj_id);
	if(! _exist_triple)
	{
		return false;
	}

	/* remove from sp2o op2s s2po o2ps s2o o2s
	 * sub2id, pre2id and obj2id will not be updated */
	(this->kvstore)->updateTupleslist_remove(_sub_id, _pre_id, _obj_id);


	int _sub_degree = (this->kvstore)->getEntityDegree(_sub_id);

	/* if subject become an isolated point, remove its corresponding entry */
	if(_sub_degree == 0)
	{
		(this->vstree)->removeEntry(_sub_id);
	}
	/* else re-calculate the signature of subject & replace that in vstree */
	else
	{
		EntityBitSet _entity_bitset;
		_entity_bitset.reset();
		this->calculateEntityBitSet(_sub_id, _entity_bitset);
		(this->vstree)->replaceEntry(_sub_id, _entity_bitset);
	}

	return true;
}
bool Database::build(const string& _rdf_file)
{
	std::string store_path = this->name;
	util::create_dir(store_path);

	std::string kv_store_path = store_path + "/kv_store";
	util::create_dir(kv_store_path);

	std::string vstree_store_path = store_path + "/vs_store";
	util::create_dir(vstree_store_path);

	cout << "begin encode RDF from : " << _rdf_file << " ..." << endl;
	this->encodeRDF(_rdf_file);
	cout << "finish encode." << endl;
	std::string _entry_file = this->getSignatureBFile();
	(this->kvstore)->open();

	cout << "begin build VS-Tree on " << _rdf_file << "..." << endl;
	(this->vstree)->buildTree(_entry_file);

	cout << "finish build VS-Tree." << endl;

	return true;
}

/* root Path of this DB + sixTuplesFile */
string Database::getSixTuplesFile()
{
	return this->getStorePath() + "/" + this->six_tuples_file;
}

/* root Path of this DB + signatureBFile */
string Database::getSignatureBFile()
{
	return this->getStorePath() + "/" + this->signature_binary_file;
}

/*
 * private methods:
 */

string Database::getStorePath()
{
	return this->name;
}


/* encode relative signature data of the query graph */
void Database::buildSparqlSignature(SPARQLquery & _sparql_q)
{
	std::vector<BasicQuery*>& _query_union = _sparql_q.getBasicQueryVec();
	for(unsigned int i_bq = 0; i_bq < _query_union.size(); i_bq ++)
	{
		BasicQuery* _basic_q = _query_union[i_bq];
		_basic_q->encodeBasicQuery(this->kvstore, _sparql_q.getQueryVar());
	}
}



bool Database::calculateEntityBitSet(int _sub_id, EntityBitSet & _bitset)
{
	int* _polist = NULL;
	int _list_len = 0;
	(this->kvstore)->getpreIDobjIDlistBysubID(_sub_id, _polist, _list_len);
	Triple _triple;
	_triple.subject = (this->kvstore)->getEntityByID(_sub_id);
	for(int i = 0; i < _list_len; i += 2)
	{
		int _pre_id = _polist[i];
		int _obj_id = _polist[i+1];
		_triple.object = (this->kvstore)->getEntityByID(_obj_id);
		if(_triple.object == "")
		{
			_triple.object = (this->kvstore)->getLiteralByID(_obj_id);
		}
		_triple.predicate = (this->kvstore)->getPredicateByID(_pre_id);
		this->encodeTriple2EntityBitSet(_bitset, &_triple);
	}
	return true;
}

/* encode Triple into SigEntry */
bool Database::encodeTriple2EntityBitSet(EntityBitSet& _bitset, const Triple* _p_triple)
{
	int _pre_id = -1;
	{
		_pre_id = (this->kvstore)->getIDByPredicate(_p_triple->predicate);
		/* checking whether _pre_id is -1 or not will be more reliable */
	}

	Signature::encodePredicate2Entity(_pre_id, _bitset, BasicQuery::EDGE_OUT);
	if(this->encode_mode == Database::ID_MODE)
	{
		/* TBD */
	}
	else
	if(this->encode_mode == Database::STRING_MODE)
	{
		Signature::encodeStr2Entity( (_p_triple->subject).c_str(), _bitset);
		Signature::encodeStr2Entity( (_p_triple->object ).c_str(), _bitset);
	}

	return true;
}

/* check whether the relative 3-tuples exist
 * usually, through sp2olist */
bool Database::exist_triple(int _sub_id, int _pre_id, int _obj_id)
{
	int* _objidlist = NULL;
	int _list_len = 0;
	(this->kvstore)->getobjIDlistBysubIDpreID(_sub_id, _pre_id, _objidlist, _list_len);
	for(int i = 0; i < _list_len; i ++)
	{
		if(_objidlist[i] == _obj_id)
		{
			delete[] _objidlist;
			return true;
		}
	}
	return false;
}

/*
 * _rdf_file denotes the path of the RDF file, where stores the rdf data
 * there are many step will be finished in this function:
 * 1. assign tuples of RDF data with id, and store the map into KVstore
 * 2. build signature of each entity
 *
 * multi-thread implementation may save lots of time
 */
bool Database::encodeRDF(const string _rdf_file)
{
	Database::log("In encodeRDF");
	int ** _p_id_tuples = NULL;
	int _id_tuples_max = 0;

	/* map sub2id and pre2id, storing in kvstore */
	this->sub2id_pre2id(_rdf_file, _p_id_tuples, _id_tuples_max);

	/* map literal2id, and encode RDF data into signature in the meantime */
	this->literal2id_RDFintoSignature(_rdf_file, _p_id_tuples, _id_tuples_max);

	/* map subid 2 objid_list  &
	 * subIDpreID 2 objid_list &
	 * subID 2 <preIDobjID>_list */
	this->s2o_sp2o_s2po(_p_id_tuples, _id_tuples_max);

	/* map objid 2 subid_list  &
	 * objIDpreID 2 subid_list &
	 * objID 2 <preIDsubID>_list */
	this->o2s_op2s_o2ps(_p_id_tuples, _id_tuples_max);

	Database::log("finish encodeRDF");

	return true;
}
/*
 *	only after we determine the entityID(subid),
 *	we can determine the literalID(objid)
 */
bool Database::sub2id_pre2id(const string _rdf_file, int**& _p_id_tuples, int & _id_tuples_max)
{
	int _id_tuples_size;;
	{/* initial */
		_id_tuples_max = 10*1000*1000;
		_p_id_tuples = new int*[_id_tuples_max];
		_id_tuples_size = 0;
		this->sub_num = 0;
		this->pre_num = 0;
		this->triples_num = 0;
		(this->kvstore)->open_entity2id(KVstore::CREATE_MODE);
		(this->kvstore)->open_id2entity(KVstore::CREATE_MODE);
		(this->kvstore)->open_predicate2id(KVstore::CREATE_MODE);
		(this->kvstore)->open_id2predicate(KVstore::CREATE_MODE);
	}

	Database::log("finish initial sub2id_pre2id");
	{/* map sub2id and pre2id */
		ifstream _fin(_rdf_file.c_str());
		if(!_fin){
			cerr << "sub2id&pre2id: Fail to open : " << _rdf_file << endl;
			exit(0);
		}

		Triple* triple_array = new Triple[DBparser::TRIPLE_NUM_PER_GROUP];

		DBparser _parser;
		/* In while(true): For sub2id and pre2id.
		 * parsed all RDF triples one group by one group
		 * when parsed out an group RDF triples
		 * for each triple
		 * assign subject with subid, and predicate with preid
		 * when get all sub2id,
		 * we can assign object with objid in next while(true)
		 * so that we can differentiate subject and object by their id
		 *  */
		Database::log("==> while(true)");
		while(true)
		{
			int parse_triple_num = 0;
			_parser.rdfParser(_fin, triple_array, parse_triple_num);
			{
				stringstream _ss;
				_ss << "finish rdfparser" << this->triples_num << endl;
				Database::log(_ss.str());
				cout << _ss.str() << endl;
			}
			if(parse_triple_num == 0){
				break;
			}

			/* Process the Triple one by one */
			for(int i = 0; i < parse_triple_num; i ++)
			{
				this->triples_num ++;

				 /* if the _id_tuples exceeds, double the space */
				if(_id_tuples_size == _id_tuples_max){
					int _new_tuples_len = _id_tuples_max * 2;
					int** _new_id_tuples = new int*[_new_tuples_len];
					memcpy(_new_id_tuples, _p_id_tuples, sizeof(int*) * _id_tuples_max);
					delete[] _p_id_tuples;
					_p_id_tuples = _new_id_tuples;
					_id_tuples_max = _new_tuples_len;
				}

				/*
				 * For subject
				 * (all subject is entity, some object is entity, the other is literal)
				 * */
				std::string _sub = triple_array[i].subject;
				int _sub_id = (this->kvstore)->getIDByEntity(_sub);
				if(_sub_id == -1){
					_sub_id = this->sub_num;
					(this->kvstore)->setIDByEntity(_sub, _sub_id);
					(this->kvstore)->setEntityByID(_sub_id, _sub);
					this->sub_num ++;
				}
				/*
				 * For predicate
				 * */
				std::string _pre = triple_array[i].predicate;
				int _pre_id = (this->kvstore)->getIDByPredicate(_pre);
				if(_pre_id == -1){
					_pre_id = this->pre_num;
					(this->kvstore)->setIDByPredicate(_pre, _pre_id);
					(this->kvstore)->setPredicateByID(_pre_id, _pre);
					this->pre_num ++;
				}

				//debug
//				{
//				    stringstream _ss;
//				    _ss << "sub: " << _sub << "\tpre: " << _pre <<endl;
//				    Database::log(_ss.str());
//				}

				/*
				 * For id_tuples
				 */
				_p_id_tuples[_id_tuples_size] = new int[3];
				_p_id_tuples[_id_tuples_size][0] = _sub_id;
				_p_id_tuples[_id_tuples_size][1] = _pre_id;
				_p_id_tuples[_id_tuples_size][2] = -1;
				_id_tuples_size ++;
			}
		}/* end while(true) for sub2id and pre2id */
		delete[] triple_array;
		_fin.close();
	}

	{/* final process */
		this->entity_num = this->sub_num;
		(this->kvstore)->release();
	}

	{
		std::stringstream _ss;
		_ss << "finish sub2id pre2id" << endl;
		_ss << "tripleNum is " << this->triples_num << endl;
		_ss << "subNum is " << this->sub_num << endl;
		_ss << "preNum is " << this->pre_num << endl;
		Database::log(_ss.str());
		cout << _ss.str() << endl;
	}

	return true;
}

/* map literal2id and encode RDF data into signature in the meantime
 * literal id begin with Database::LITERAL_FIRST_ID */
bool Database::literal2id_RDFintoSignature(const string _rdf_file, int** _p_id_tuples, int _id_tuples_max)
{
	Database::log("IN literal2id...");

	EntityBitSet* _entity_bitset = new EntityBitSet[this->sub_num];
	for(int i = 0; i < this->sub_num; i ++){
		_entity_bitset[i].reset();
	}

	(this->kvstore)->open_id2literal(KVstore::CREATE_MODE);
	(this->kvstore)->open_literal2id(KVstore::CREATE_MODE);
	(this->kvstore)->open_entity2id(KVstore::READ_WRITE_MODE);

	/*  map obj2id */
	ifstream _fin(_rdf_file.c_str());
	if(!_fin){
		cerr << "obj2id: Fail to open : " << _rdf_file << endl;
		exit(0);
	}

	std::string _six_tuples_file = this->getSixTuplesFile();
	std::ofstream _six_tuples_fout(_six_tuples_file.c_str());
	if(! _six_tuples_fout){
		cerr << "obj2id: failed to open: " << _six_tuples_file << endl;
		exit(0);
	}

	Triple* triple_array = new Triple[DBparser::TRIPLE_NUM_PER_GROUP];

	DBparser _parser;
	this->entity_num = this->sub_num;
	int _i_tuples = 0;
	EntityBitSet _tmp_bitset;
	/* In while(true): For obj2id .
	 * parsed all RDF triples one group by one group
	 * when parsed out an group RDF triples
	 * for each triple
	 * assign object with objid
	 *  */
	Database::log("literal2id: while(true)");
	while(true)
	{
		/* get next group of triples from rdfParser */
		int parse_triple_num = 0;
		_parser.rdfParser(_fin, triple_array, parse_triple_num);
		{
			stringstream _ss;
			_ss << "finish rdfparser" << _i_tuples << endl;
			Database::log(_ss.str());
			cout << _ss.str() << endl;
		}
		if(parse_triple_num == 0){
			break;
		}

		/* Process the Triple one by one */
		for(int i = 0; i < parse_triple_num; i ++)
		{
			/*
			 * For object(literal)
			 * */
			std::string _obj = triple_array[i].object;
			/* check whether obj is an entity or not
			 * if not, obj is a literal and assign it with a literal id */
			int _obj_id = (this->kvstore)->getIDByEntity(_obj);

			if(Database::debug_2)
			{
			}

			/* if obj is an literal */
			if(_obj_id == -1)
			{
				int _literal_id = (this->kvstore)->getIDByLiteral(_obj);
				/* if this literal does not exist before */
				if(_literal_id == -1)
				{
					int _new_literal_id = Database::LITERAL_FIRST_ID + (this->literal_num);
					(this->kvstore)->setIDByLiteral(_obj, _new_literal_id);
					(this->kvstore)->setLiteralByID(_new_literal_id, _obj);
					this->literal_num ++;
					_obj_id = _new_literal_id;
				}
				else
				{
					_obj_id = _literal_id;
				}
			}

//			{
//				stringstream _ss;
//				_ss << "object: " << _obj << " has id " << _obj_id << endl;
//				Database::log(_ss.str());
//			}

			_p_id_tuples[_i_tuples][2] = _obj_id;

			/*
			 *  save six tuples
			 *  */
			{
				_six_tuples_fout << _p_id_tuples[_i_tuples][0] << '\t'
								 << _p_id_tuples[_i_tuples][1] << '\t'
								 << _p_id_tuples[_i_tuples][2] << '\t'
								 << triple_array[i].subject   << '\t'
								 << triple_array[i].predicate << '\t'
								 << triple_array[i].object    << endl;
			}

			/*
			 * calculate entity signature
			 */
			int _sub_id = _p_id_tuples[_i_tuples][0];
			int _pre_id = _p_id_tuples[_i_tuples][1];

			_tmp_bitset.reset();
			Signature::encodePredicate2Entity(_pre_id, _tmp_bitset, BasicQuery::EDGE_OUT);
			Signature::encodeStr2Entity((triple_array[i].object).c_str(), _tmp_bitset);
			_entity_bitset[_sub_id] |= _tmp_bitset;

			if(this->objIDIsEntityID(_obj_id))
			{
				_tmp_bitset.reset();
				Signature::encodePredicate2Entity(_pre_id, _tmp_bitset, BasicQuery::EDGE_IN);
				Signature::encodeStr2Entity((triple_array[i].subject).c_str(), _tmp_bitset);
				_entity_bitset[_obj_id] |= _tmp_bitset;
			}

			_i_tuples ++;
		}

	}/* end for while(true) */

	cout << "end for while" << endl;
	delete[] triple_array;
	_six_tuples_fout.close();
	_fin.close();

	(this->kvstore)->release();

	{/* save all entity_signature into binary file */
		string _sig_binary_file = this->getSignatureBFile();
		FILE* _sig_fp = fopen(_sig_binary_file.c_str(), "wb");
		if(_sig_fp == NULL){
			cerr << "Failed to open : " << _sig_binary_file << endl;
		}

		EntityBitSet _all_bitset;
		for(int i = 0; i < this->sub_num; i ++)
		{
			SigEntry* _sig = new SigEntry(EntitySig(_entity_bitset[i]), i);

			//debug
//			if(i == 0 || i == 2)
//			{
//				stringstream _ss;
//				_ss << "encodeRDF: " << i << " =" << _sig->getEntitySig().entityBitSet << endl;
//				Database::log(_ss.str());
//			}

			fwrite(_sig, sizeof(SigEntry), 1, _sig_fp);
			_all_bitset |= _entity_bitset[i];
			delete _sig;
		}
		fclose(_sig_fp);

		//debug
//		{
//		    int sub_id = (this->kvstore)->getIDByEntity("<http://www.Department0.University0.edu>");
//		    stringstream _ss;
//		    _ss << "sub: " << "<http://www.Department0.University0.edu>(" << sub_id << ")" << endl;
//		    _ss << "bitset: " << Signature::BitSet2str(_entity_bitset[sub_id]) << endl;
//		    Database::log(_ss.str());
//		}

		delete[] _entity_bitset;
	}

	Database::log("OUT literal2id...");

	return true;
}

/* map subid 2 objid_list  &
 * subIDpreID 2 objid_list &
 * subID 2 <preIDobjID>_list */
bool Database::s2o_sp2o_s2po(int** _p_id_tuples, int _id_tuples_max)
{
	qsort(_p_id_tuples, this->triples_num, sizeof(int*), Database:: _spo_cmp);
	int* _oidlist_s = NULL;
	int* _oidlist_sp = NULL;
	int* _pidoidlist_s = NULL;
	int _oidlist_s_len = 0;
	int _oidlist_sp_len = 0;
	int _pidoidlist_s_len = 0;
	/* only _oidlist_s will be assigned with space
	 * _oidlist_sp is always a part of _oidlist_s
	 * just a pointer is enough
	 *  */
	int _oidlist_max = 0;
	int _pidoidlist_max = 0;

	/* true means next sub is a different one from the previous one */
	bool _sub_change = true;

	/* true means next <sub,pre> is different from the previous pair */
	bool _sub_pre_change = true;

	/* true means next pre is different from the previous one */
	bool _pre_change = true;

	Database::log("finish s2p_sp2o_s2po initial");

	(this->kvstore)->open_subid2objidlist(KVstore::CREATE_MODE);
	(this->kvstore)->open_subIDpreID2objIDlist(KVstore::CREATE_MODE);
	(this->kvstore)->open_subID2preIDobjIDlist(KVstore::CREATE_MODE);

	for(int i = 0; i < this->triples_num; i ++)
	{
		if(_sub_change)
		{
			/* oidlist */
			_oidlist_max = 1000;
			_oidlist_s = new int[_oidlist_max];
			_oidlist_sp = _oidlist_s;
			_oidlist_s_len = 0;
			_oidlist_sp_len = 0;
			/* pidoidlist */
			_pidoidlist_max = 1000 * 2;
			_pidoidlist_s = new int[_pidoidlist_max];
			_pidoidlist_s_len = 0;
		}
		/* enlarge the space when needed */
		if(_oidlist_s_len == _oidlist_max)
		{
			_oidlist_max *= 10;
			int * _new_oidlist_s = new int[_oidlist_max];
			memcpy(_new_oidlist_s, _oidlist_s, sizeof(int) * _oidlist_s_len);
			/* (_oidlist_sp-_oidlist_s) is the offset of _oidlist_sp */
			_oidlist_sp = _new_oidlist_s + (_oidlist_sp-_oidlist_s);
			delete[] _oidlist_s;
			_oidlist_s = _new_oidlist_s;
		}

		/* enlarge the space when needed */
		if(_pidoidlist_s_len == _pidoidlist_max)
		{
			_pidoidlist_max *= 10;
			int* _new_pidoidlist_s = new int[_pidoidlist_max];
			memcpy(_new_pidoidlist_s, _pidoidlist_s, sizeof(int) * _pidoidlist_s_len);
			delete[] _pidoidlist_s;
			_pidoidlist_s = _new_pidoidlist_s;
		}

		int _sub_id = _p_id_tuples[i][0];
		int _pre_id = _p_id_tuples[i][1];
		int _obj_id = _p_id_tuples[i][2];
//		{
//			stringstream _ss;
//			_ss << _sub_id << "\t" << _pre_id << "\t" << _obj_id << endl;
//			Database::log(_ss.str());
//		}

		/* add objid to list */
		_oidlist_s[_oidlist_s_len] = _obj_id;

		/* if <subid, preid> changes, _oidlist_sp should be adjusted */
		if(_sub_pre_change){
			_oidlist_sp = _oidlist_s + _oidlist_s_len;
		}

		_oidlist_s_len ++;
		_oidlist_sp_len ++;

		/* add <preid, objid> to list */
		_pidoidlist_s[_pidoidlist_s_len] = _pre_id;
		_pidoidlist_s[_pidoidlist_s_len+1] = _obj_id;
		_pidoidlist_s_len += 2;


		/* whether sub in new triple changes or not */
		_sub_change = (i+1 == this->triples_num) ||
					(_p_id_tuples[i][0] != _p_id_tuples[i+1][0]);

		/* whether pre in new triple changes or not */
		_pre_change = (i+1 == this->triples_num) ||
					(_p_id_tuples[i][1] != _p_id_tuples[i+1][1]);

		/* whether <sub,pre> in new triple changes or not */
		_sub_pre_change = _sub_change || _pre_change;

		if(_sub_pre_change)
		{
			(this->kvstore)->setobjIDlistBysubIDpreID(_sub_id, _pre_id, _oidlist_sp, _oidlist_sp_len);
			_oidlist_sp = NULL;
			_oidlist_sp_len = 0;
		}

		if(_sub_change)
		{
			/* map subid 2 objidlist */
			util::sort(_oidlist_s, _oidlist_s_len);
			(this->kvstore)->setobjIDlistBysubID(_sub_id, _oidlist_s, _oidlist_s_len);
			delete[] _oidlist_s;
			_oidlist_s = NULL;
			_oidlist_sp = NULL;
			_oidlist_s_len = 0;

			/* map subid 2 preid&objidlist */
			(this->kvstore)->setpreIDobjIDlistBysubID(_sub_id, _pidoidlist_s, _pidoidlist_s_len);
			delete[] _pidoidlist_s;
			_pidoidlist_s = NULL;
			_pidoidlist_s_len = 0;
		}

	}/* end for( 0 to this->triple_num)  */

	(this->kvstore)->release();

	Database::log("OUT s2po...");

	return true;
}

/* map objid 2 subid_list  &
 * objIDpreID 2 subid_list &
 * objID 2 <preIDsubID>_list */
bool Database::o2s_op2s_o2ps(int** _p_id_tuples, int _id_tuples_max)
{
	Database::log("IN o2ps...");

	qsort(_p_id_tuples, this->triples_num, sizeof(int**), Database::_ops_cmp);
	int* _sidlist_o = NULL;
	int* _sidlist_op = NULL;
	int* _pidsidlist_o = NULL;
	int _sidlist_o_len = 0;
	int _sidlist_op_len = 0;
	int _pidsidlist_o_len = 0;
	/* only _sidlist_o will be assigned with space
	 * _sidlist_op is always a part of _sidlist_o
	 * just a pointer is enough */
	int _sidlist_max = 0;
	int _pidsidlist_max = 0;

	/* true means next obj is a different one from the previous one */
	bool _obj_change = true;

	/* true means next <obj,pre> is different from the previous pair */
	bool _obj_pre_change = true;

	/* true means next pre is a different one from the previous one */
	bool _pre_change = true;

	(this->kvstore)->open_objid2subidlist(KVstore::CREATE_MODE);
	(this->kvstore)->open_objIDpreID2subIDlist(KVstore::CREATE_MODE);
	(this->kvstore)->open_objID2preIDsubIDlist(KVstore::CREATE_MODE);

	for(int i = 0; i < this->triples_num; i ++)
	{
		if(_obj_change)
		{
			/* sidlist */
			_sidlist_max = 1000;
			_sidlist_o = new int[_sidlist_max];
			_sidlist_op = _sidlist_o;
			_sidlist_o_len = 0;
			_sidlist_op_len = 0;
			/* pidsidlist */
			_pidsidlist_max = 1000 * 2;
			_pidsidlist_o = new int[_pidsidlist_max];
			_pidsidlist_o_len = 0;
		}
		/* enlarge the space when needed */
		if(_sidlist_o_len == _sidlist_max)
		{
			_sidlist_max *= 10;
			int * _new_sidlist_o = new int[_sidlist_max];
			memcpy(_new_sidlist_o, _sidlist_o, sizeof(int)*_sidlist_o_len);
			/* (_sidlist_op-_sidlist_o) is the offset of _sidlist_op */
			_sidlist_op = _new_sidlist_o + (_sidlist_op-_sidlist_o);
			delete[] _sidlist_o;
			_sidlist_o = _new_sidlist_o;
		}

		/* enlarge the space when needed */
		if(_pidsidlist_o_len == _pidsidlist_max)
		{
			_pidsidlist_max *= 10;
			int* _new_pidsidlist_o = new int[_pidsidlist_max];
			memcpy(_new_pidsidlist_o, _pidsidlist_o, sizeof(int) * _pidsidlist_o_len);
			delete[] _pidsidlist_o;
			_pidsidlist_o = _new_pidsidlist_o;
		}

		int _sub_id = _p_id_tuples[i][0];
		int _pre_id = _p_id_tuples[i][1];
		int _obj_id = _p_id_tuples[i][2];

		/* add subid to list */
		_sidlist_o[_sidlist_o_len] = _sub_id;

		/* if <objid, preid> changes, _sidlist_op should be adjusted */
		if(_obj_pre_change){
			_sidlist_op = _sidlist_o + _sidlist_o_len;
		}

		_sidlist_o_len ++;
		_sidlist_op_len ++;

		/* add <preid, subid> to list */
		_pidsidlist_o[_pidsidlist_o_len] = _pre_id;
		_pidsidlist_o[_pidsidlist_o_len+1] = _sub_id;;
		_pidsidlist_o_len += 2;

		/* whether sub in new triple changes or not */
		_obj_change = (i+1 == this->triples_num) ||
					(_p_id_tuples[i][2] != _p_id_tuples[i+1][2]);

		/* whether pre in new triple changes or not */
		_pre_change = (i+1 == this->triples_num) ||
					(_p_id_tuples[i][1] != _p_id_tuples[i+1][1]);

		/* whether <sub,pre> in new triple changes or not */
		_obj_pre_change = _obj_change || _pre_change;

		if(_obj_pre_change)
		{
			(this->kvstore)->setsubIDlistByobjIDpreID(_obj_id, _pre_id, _sidlist_op, _sidlist_op_len);
			_sidlist_op = NULL;
			_sidlist_op_len = 0;
		}

		if(_obj_change)
		{
			/* map objid 2 subidlist */
			util::sort(_sidlist_o, _sidlist_o_len);
			(this->kvstore)->setsubIDlistByobjID(_obj_id, _sidlist_o, _sidlist_o_len);
			delete[] _sidlist_o;
			_sidlist_o = NULL;
			_sidlist_op = NULL;
			_sidlist_o_len = 0;

			/* map objid 2 preid&subidlist */
			(this->kvstore)->setpreIDsubIDlistByobjID(_obj_id, _pidsidlist_o, _pidsidlist_o_len);
			delete[] _pidsidlist_o;
			_pidsidlist_o = NULL;
			_pidsidlist_o_len = 0;
		}

	}/* end for( 0 to this->triple_num)  */

	(this->kvstore)->release();

	Database::log("OUT o2ps...");

	return true;
}

/* compare function for qsort */
int Database::_spo_cmp(const void* _a, const void* _b)
{
	int** _p_a = (int**)_a;
	int** _p_b = (int**)_b;

	{/* compare subid first */
		int _sub_id_a = (*_p_a)[0];
		int _sub_id_b = (*_p_b)[0];
		if(_sub_id_a != _sub_id_b){
			return _sub_id_a - _sub_id_b;
		}
	}

	{/* then preid */
		int _pre_id_a = (*_p_a)[1];
		int _pre_id_b = (*_p_b)[1];
		if(_pre_id_a != _pre_id_b){
			return _pre_id_a - _pre_id_b;
		}
	}

	{/* objid at last */
		int _obj_id_a = (*_p_a)[2];
		int _obj_id_b = (*_p_b)[2];
		if(_obj_id_a != _obj_id_b){
			return _obj_id_a - _obj_id_b;
		}
	}
	return 0;
}
/* compare function for qsort */
int Database::_ops_cmp(const void* _a, const void* _b)
{
	int** _p_a = (int**)_a;
	int** _p_b = (int**)_b;
	{/* compare objid first */
		int _obj_id_a = (*_p_a)[2];
		int _obj_id_b = (*_p_b)[2];
		if(_obj_id_a != _obj_id_b){
			return _obj_id_a - _obj_id_b;
		}
	}

	{/* then preid */
		int _pre_id_a = (*_p_a)[1];
		int _pre_id_b = (*_p_b)[1];
		if(_pre_id_a != _pre_id_b){
			return _pre_id_a - _pre_id_b;
		}
	}

	{/* subid at last */
		int _sub_id_a = (*_p_a)[0];
		int _sub_id_b = (*_p_b)[0];
		if(_sub_id_a != _sub_id_b){
			return _sub_id_a - _sub_id_b;
		}
	}

	return 0;
}

bool Database::objIDIsEntityID(int _id)
{
	return _id < Database::LITERAL_FIRST_ID;
}


bool Database::join
(vector<int*>& _result_list, int _var_id, int _pre_id, int _var_id2, const char _edge_type,
 int _var_num, bool shouldAddLiteral, IDList& _can_list)
 {
//	std::cout << "*****Join [" << _var_id << "]\tpre:" << _pre_id << "\t[" << _var_id2 << "]\t"
//			<< "result before: " << _result_list.size() << "\t etype:" << _edge_type
//			<< std::endl;
	{
//		stringstream _ss;
//		_ss << "\n\n\n\n*****Join [" << _var_id << "]\tpre:" << _pre_id << "\t[" << _var_id2 << "]\t"
//				<< "result before: " << _result_list.size() << "\t etype:" << _edge_type
//				<< std::endl;
//		Database::log(_ss.str());
	}
//	cout << _can_list.to_str() << endl;
	int* id_list;
	int id_list_len;
	vector<int*> new_result_list;
	new_result_list.clear();

	vector<int*>::iterator itr = _result_list.begin();

	bool has_preid = (_pre_id >= 0);

	for ( ; itr != _result_list.end(); itr++)
	{
		int* itr_result = (*itr);
		if (itr_result[_var_num] == -1)
		{
			continue;
		}

		if (_can_list.size()==0 && !shouldAddLiteral)
		{
		    itr_result[_var_num] = -1;
		    continue;
		}

//		std::string _can_str = (this->kvstore)->getEntityByID((itr_result[_var_id]));
//		std::cout << "\t\t v[" << _var_id << "] has: ["
//				<< _can_str << ", " << (*itr)[_var_id] << "]"
//				<< std::endl;
//		{
//			stringstream _ss;
//			_ss << "\t\t v[" << _var_id << "] has: ["
//					<< _can_str << ", " << (*itr)[_var_id] << "]"
//					<< std::endl;
//			Database::log(_ss.str());
//		}

		if (has_preid)
		{
			if (_edge_type == BasicQuery::EDGE_IN)
			{
				kvstore->getsubIDlistByobjIDpreID(itr_result[_var_id],
						_pre_id, id_list, id_list_len);
			}
			else
			{
				kvstore->getobjIDlistBysubIDpreID(itr_result[_var_id],
						_pre_id, id_list, id_list_len);
			}

		}
		else
		{
			if (_edge_type == BasicQuery::EDGE_IN)
			{
//				std::cout << "\t\to2s" << std::endl;
				kvstore->getsubIDlistByobjID(itr_result[_var_id],
						id_list, id_list_len);
			}
			else
			{
				kvstore->getobjIDlistBysubID(itr_result[_var_id],
						id_list, id_list_len);
			}
		}

		if (id_list_len == 0)
		{
			itr_result[_var_num] = -1;
			continue;
		}
//		std::cout << "\t\tid_list_len: " << id_list_len << std::endl << "\t\t";
//		for(int i = 0; i < id_list_len; i ++){
//			cout << "[" << id_list[i] << "] ";
//		}
//		cout << endl;
		{
//			stringstream _ss;
//			_ss << "\t\tid_list_len: " << id_list_len << std::endl << "\t\t";
//			for(int i = 0; i < id_list_len; i ++){
//				_ss << "[" << id_list[i] << ", " << this->kvstore->getEntityByID(id_list[i])<< "] ";
//			}
//			_ss << endl;
//			Database::log(_ss.str());
		}

		bool no_any_match_yet = true;
		stringstream _tmp_ss;
		for (int i = 0; i < id_list_len; i++)
		{
		    bool found_in_id_list = _can_list.bsearch_uporder(id_list[i]) >= 0;
		    bool should_add_this_literal = shouldAddLiteral && !this->objIDIsEntityID(id_list[i]);

		    // if we found this element(entity/literal) in var1's candidate list,
		    // or this is a literal element and var2 is a free literal variable,
		    // we should add this one to result array.
			if (found_in_id_list || should_add_this_literal)
			{
				if (no_any_match_yet)
				{
					no_any_match_yet = false;
					itr_result[_var_id2] = id_list[i];
//					_tmp_ss << "[first, " << id_list[i] << ", "
//							<< this->kvstore->getEntityByID(id_list[i]) << "\n";
//					cout << "\t\tfirst" ;
//					cout << "\t\tpair : " << id_list[i] << endl;
				}
				else
				{
					int* result = new int[_var_num + 1];
					memcpy(result, itr_result,
							sizeof(int) * (_var_num + 1));
					result[_var_id2] = id_list[i];
					new_result_list.push_back(result);
//					cout << "\t\tpair : " << result[_var_id2] << endl;
//					cout << "\t\t new result has size " << new_result_list.size() << endl;
					{
//						_tmp_ss << "\t\tp: [" << result[_var_id2] << ", " << this->kvstore->getEntityByID(result[_var_id2])<< "]\t";
					}
				}
			}
		}

		if(no_any_match_yet)
		{
//			_tmp_ss << "no-match" << endl;
			itr_result[_var_num] = -1;
//			Database::log(_tmp_ss.str());
		}
//		_tmp_ss << "match" << endl;
//		Database::log(_tmp_ss.str());

		delete[] id_list;
	}
	if (!new_result_list.empty()) {
		vector<int*>::iterator _begin = new_result_list.begin();
		vector<int*>::iterator _end = new_result_list.end();
		_result_list.insert(_result_list.end(), _begin, _end);
	}



	int invalid_num = 0;
	for(int i = 0; i < _result_list.size(); i ++)
	{
		if(_result_list[i][_var_num] == -1)
		{
			invalid_num ++;
		}
	}

//	cout << "\t\tresult size: " << _result_list.size() << " invalid:" << invalid_num << endl;
	{
//		stringstream _ss;
//		_ss  << "\t\tresult size: " << _result_list.size() << " invalid:" << invalid_num << endl;
//		for(int i = 0; i < _result_list.size(); i ++)
//		{
//			for(int j = 0; j <= _var_num; j ++){
//				_ss << "[" << this->kvstore->getEntityByID(_result_list[i][j]) << "("
//						<< _result_list[i][j] << ")] ";
//			}
//			_ss << "\n";
//		}
//		Database::log(_ss.str());
	}
//	cout << _result_list[0][0] << " & " << _result_list[0][1] << endl;
	std::cout << "*****Join done" << std::endl;
	return true;
}

bool Database::select(vector<int*>& _result_list,int _var_id,int _pre_id,int _var_id2,const char _edge_type,int _var_num)
{
	cout << "*****In select" << endl;

	int* id_list;
	int id_list_len;

	vector<int*>::iterator itr = _result_list.begin();
	for ( ;	itr != _result_list.end(); itr++)
	{
		int* itr_result = (*itr);
		if (itr_result[_var_num] == -1) {
			continue;
		}

		bool ret = false;
		if (_pre_id >= 0)
		{
			if (_edge_type == BasicQuery::EDGE_IN)
			{
				kvstore->getsubIDlistByobjIDpreID(itr_result[_var_id],
						_pre_id, id_list, id_list_len);
			}
			else
			{
				kvstore->getobjIDlistBysubIDpreID(itr_result[_var_id],
						_pre_id, id_list, id_list_len);

			}
		}
		else
		{
			if (_edge_type == BasicQuery::EDGE_IN)
			{
				kvstore->getsubIDlistByobjID(itr_result[_var_id],
						id_list, id_list_len);
			}
			else
			{
				kvstore->getobjIDlistBysubID(itr_result[_var_id],
						id_list, id_list_len);
			}
		}

		if (id_list_len == 0)
		{
			itr_result[_var_num] = -1;
			continue;
		}

		if (util::bsearch_int_uporder(itr_result[_var_id2], id_list,
				id_list_len) == -1)
		{
			itr_result[_var_num] = -1;
		}
		delete[] id_list;
	}


	int invalid_num = 0;
	for(int i = 0; i < _result_list.size(); i ++)
	{
		if(_result_list[i][_var_num] == -1)
		{
			invalid_num ++;
		}
	}

	cout << "\t\tresult size: " << _result_list.size() << " invalid:" << invalid_num << endl;
//
	cout << "*****Select done" << endl;
	return true;
}

/*
 * join on the vector of CandidateList, available after retrieve from the VSTREE
 * and store the resut in _result_set
 *  */
bool Database::join(SPARQLquery& _sparql_query)
{

	int basic_query_num=_sparql_query.getBasicQueryNum();

	//join each basic query
	for (int i=0; i< basic_query_num; i++){
		cout<<"Basic query "<<i<<endl;
		BasicQuery* basic_query;
		basic_query=&(_sparql_query.getBasicQuery(i));
		long begin = util::get_cur_time();
		this->filter_before_join(basic_query);
		long after_filter = util::get_cur_time();
		cout << "after filter_before_join: used " << (after_filter-begin) << " ms" << endl;
		this->add_literal_candidate(basic_query);
		long after_add_literal = util::get_cur_time();
		cout << "after add_literal_candidate: used " << (after_add_literal-after_filter) << " ms" << endl;
		this->join_basic(basic_query);
		long after_joinbasic = util::get_cur_time();
		cout << "after join_basic : used " << (after_joinbasic-after_add_literal) << " ms" << endl;
		this->only_pre_filter_after_join(basic_query);
		long after_pre_filter_after_join = util::get_cur_time();
		cout << "after only_pre_filter_after_join : used " << (after_pre_filter_after_join-after_joinbasic) << " ms" << endl;
	}
	return true;
}

bool Database::join_basic(BasicQuery* basic_query)
{
	cout << "IIIIIIN join basic" << endl;

	int var_num = basic_query->getVarNum();
	int triple_num = basic_query->getTripleNum();

	//mark dealed_id_list and dealed_triple, 0 not processed, 1 for processed
	bool* dealed_id_list = new bool[var_num];
	bool* dealed_triple = new bool[triple_num];
	memset(dealed_id_list, 0, sizeof(bool) * var_num);
	memset(dealed_triple, 0, sizeof(bool) * triple_num);

	int start_var_id = basic_query->getVarID_FirstProcessWhenJoin();
	int start_var_size = basic_query->getCandidateSize(start_var_id);

	// initial p_result_list, push min_var_list in
	vector<int*>* p_result_list = &basic_query->getResultList();
	p_result_list->clear();

	//start_var_size == 0  no answer in this basic query
	if (start_var_size == 0)
	{
		return false;
	}

	//debug
	{
	    stringstream _ss;
	    _ss << "start_var_size=" << start_var_size << endl;
	    _ss << "star_var=" << basic_query->getVarName(start_var_id) << "(var[" << start_var_id << "])" << endl;
	    Database::log(_ss.str());
	}

	IDList* p_min_var_list = &(basic_query->getCandidateList(start_var_id));
	for (int i = 0; i < start_var_size; i++)
	{
		int* result_var = new int[var_num + 1];
		memset(result_var, 0, sizeof(int) * (var_num + 1));
		result_var[start_var_id] = p_min_var_list->getID(i);
		p_result_list->push_back(result_var);
	}

	//BFS search

	stack<int> var_stack;
	var_stack.push(start_var_id);
	dealed_id_list[start_var_id] = true;
	while (!var_stack.empty())
	{
		int var_id = var_stack.top();
		var_stack.pop();
		int var_degree = basic_query->getVarDegree(var_id);
		for (int i = 0; i < var_degree; i++)
		{
		    // each triple/edge need to be processed only once.
		    int edge_id = basic_query->getEdgeID(var_id, i);
		    if (dealed_triple[edge_id])
		    {
		        continue;
		    }
			int var_id2 = basic_query->getEdgeNeighborID(var_id, i);
			if (var_id2 == -1)
			{
				continue;
			}

			int pre_id = basic_query->getEdgePreID(var_id, i);
			char edge_type = basic_query->getEdgeType(var_id, i);
			IDList& can_list = basic_query->getCandidateList(var_id2);

			if (!dealed_id_list[var_id2])
			{
				//join
			    bool shouldVar2AddLiteralCandidateWhenJoin = basic_query->isFreeLiteralVariable(var_id2) &&
			                                                 !basic_query->isAddedLiteralCandidate(var_id2);

				join(*p_result_list, var_id, pre_id, var_id2, edge_type,
					 var_num, shouldVar2AddLiteralCandidateWhenJoin, can_list);
				var_stack.push(var_id2);
				basic_query->setAddedLiteralCandidate(var_id2);
				dealed_id_list[var_id2] = true;
			}
			else
			{
				//select
				select(*p_result_list, var_id, pre_id, var_id2, edge_type,var_num);
			}

			dealed_triple[edge_id] = true;
		}
	}

	basic_query->dupRemoval_invalidRemoval();

	vector<int*> &result = basic_query->getResultList();
	int result_size = result.size();
	std::cout << "\t\tFinal result:" << result_size << std::endl;

	cout << "OOOOOUT join basic" << endl;
	return true;
}

void Database::filter_before_join(BasicQuery* basic_query)
{

	cout << "*****IIIIIIN filter_before_join" << endl;

	int var_num = 0;
	var_num = basic_query->getVarNum();

	for (int i = 0; i < var_num; i++)
	{
		std::cout << "\tVar" << i << " " << basic_query->getVarName(i) << std::endl;
		IDList &can_list = basic_query->getCandidateList(i);
		cout << "\t\tsize of canlist before filter: " << can_list.size() << endl;
		// must sort before using binary search.
		can_list.sort();

		long begin = util::get_cur_time();
		this->literal_edge_filter(basic_query, i);
		long after_literal_edge_filter = util::get_cur_time();
		cout << "\t\tliteral_edge_filter: used " << (after_literal_edge_filter-begin) << " ms" << endl;
//		this->preid_filter(basic_query, i);
//		long after_preid_filter = util::get_cur_time();
//		cout << "\t\tafter_preid_filter: used " << (after_preid_filter-after_literal_edge_filter) << " ms" << endl;
		cout << "\t\t[" << i << "] after filter, candidate size = " << can_list.size() << endl << endl << endl;

		//debug
//		{
//			stringstream _ss;
//			for(int i = 0; i < can_list.size(); i ++)
//			{
//				string _can = this->kvstore->getEntityByID(can_list[i]);
//				_ss << "[" << _can << ", " << can_list[i] << "]\t";
//			}
//			_ss << endl;
//			Database::log(_ss.str());
//			cout << can_list.to_str() << endl;
//		}
	}

	cout << "OOOOOOUT filter_before_join" << endl;
}

void Database::literal_edge_filter(BasicQuery* basic_query, int _var_i)
{
    Database::log("IN literal_edge_filter"); //debug

	int var_degree = basic_query->getVarDegree(_var_i);
	for(int j = 0; j < var_degree; j ++)
	{
		int neighbor_id = basic_query->getEdgeNeighborID(_var_i, j);
		//	continue;
		cout << "\t\t\tneighbor_id=" << neighbor_id << endl;
		if (neighbor_id != -1)
		{
			continue;
		}

		char edge_type = basic_query->getEdgeType(_var_i, j);
		int triple_id = basic_query->getEdgeID(_var_i, j);
		Triple triple = basic_query->getTriple(triple_id);
		std::string neighbor_name;

		if (edge_type == BasicQuery::EDGE_OUT)
		{
			neighbor_name = triple.object;
		}
		else
		{
			neighbor_name = triple.subject;
		}

		/* if neightbor is an var, but not in select
		 * then, if its degree is 1, it has none contribution to filter
		 * only its sole edge property(predicate) makes sense
		 * we should make sure that current candidateVar has an edge matching the predicate
		 *  */
		bool only_preid_filter = (basic_query->isOneDegreeNotSelectVar(neighbor_name));
		if(only_preid_filter)
		{
			continue;
		}

		int pre_id = basic_query->getEdgePreID(_var_i, j);
		IDList &_list = basic_query->getCandidateList(_var_i);

		int lit_id = (this->kvstore)->getIDByEntity(neighbor_name);
		if(lit_id == -1)
		{
			lit_id = (this->kvstore)->getIDByLiteral(neighbor_name);
		}

	//			std::cout << "\t\tedge[" << j << "] "<< lit_string << " has id " << lit_id << "";
	//			std::cout << " preid:" << pre_id << " type:" << edge_type
	//					<< std::endl;
		{
	//				stringstream _ss;
	//				_ss << "\t\tedge[" << j << "] "<< lit_string << " has id " << lit_id << "";
	//				_ss << " preid:" << pre_id << " type:" << edge_type
	//						<< std::endl;
	//				Database::log(_ss.str());
		}

		int id_list_len = 0;
		int* id_list = NULL;
		if (pre_id >= 0)
		{
			if (edge_type == BasicQuery::EDGE_OUT)
			{
				(this->kvstore)->getsubIDlistByobjIDpreID(lit_id, pre_id, id_list, id_list_len);
			}
			else
			{
			    (this->kvstore)->getobjIDlistBysubIDpreID(lit_id, pre_id, id_list, id_list_len);
			}
		}
		else
		{
			if (edge_type == BasicQuery::EDGE_OUT)
			{
			    (this->kvstore)->getsubIDlistByobjID(lit_id, id_list, id_list_len);
			}
			else
			{
			    (this->kvstore)->getobjIDlistBysubID(lit_id, id_list, id_list_len);
			}
		}

		/*
		//debug
		{
		    stringstream _ss;
		    _ss << "id_list: ";
		    for (int i=0;i<id_list_len;i++)
		    {
		        _ss << "[" << id_list[i] << "]\t";
		    }
		    _ss<<endl;
		    Database::log(_ss.str());
		}
		*/

		if(id_list_len == 0)
		{
		    _list.clear();
		    delete []id_list;
			return;
		}
	//			std::cout << "\t\t can:" << can_list.to_str() << endl;
	//			cout << "\t\t idlist has :";
	//			for(int i_ = 0; i_ < id_list_len; i_ ++)
	//			{
	//				cout << "[" << id_list[i_] << "]\t";
	//			}
	//			cout << endl;

		_list.intersectList(id_list, id_list_len);
		delete []id_list;
	}

	Database::log("OUT literal_edge_filter"); //debug
}

/* this part can be omited or improved if the encode way of predicate
 * is good enough
 * also, we can decide whether we need run this part (if there are predicates encode overlap)
 * by var_i's edge in queryGraph,
 *
 *
 * for each edge e of var_i,
 * if neightbor on e is an var, but not in select
 * then, if the var's degree is 1, it has none contribution to filter
 * only its sole edge property(predicate) makes sense
 * we should make sure that var_i has an edge matching the predicate
 * so this function will do the filtering
 * TBD:
 * if pre_id = -1,
 * it means the entity id must has at least one  edge*/
void Database::preid_filter(BasicQuery* basic_query, int _var_i)
{
	//IDList & _list, int _pre_id, char _edge_type
	for (int j = 0; j < basic_query->getVarDegree(_var_i); j++)
	{
		int neighbor_id = basic_query->getEdgeNeighborID(_var_i, j);
		//	continue;
		cout << "\t\t\tneighbor_id=" << neighbor_id << endl;
		if (neighbor_id != -1)
		{
			continue;
		}

		char edge_type = basic_query->getEdgeType(_var_i, j);
		int triple_id = basic_query->getEdgeID(_var_i, j);
		Triple triple = basic_query->getTriple(triple_id);
		std::string neighbor_name;

		if (edge_type == BasicQuery::EDGE_OUT)
		{
			neighbor_name = triple.object;
		}
		else
		{
			neighbor_name = triple.subject;
		}

		/* if neightbor is an var, but not in select
		 * then, if its degree is 1, it has none contribution to filter
		 * only its sole edge property(predicate) makes sense
		 * we should make sure that current candidateVar has an edge matching the predicate
		 *  */
		bool only_preid_filter = (basic_query->isOneDegreeNotSelectVar(neighbor_name));
		if (!only_preid_filter)
		{
			continue;
		}

		int pre_id = basic_query->getEdgePreID(_var_i, j);
		IDList& _list = basic_query->getCandidateList(_var_i);
		int* remain_list = new int[_list.size()];
		int remain_len = 0;
		int _entity_id = -1;
		int* pair_list = NULL;
		int pair_len = 0;

		for (int i = 0; i < _list.size(); i++)
		{
			_entity_id = _list[i];
			if (edge_type == BasicQuery::EDGE_IN)
			{
				(this->kvstore)->getpreIDsubIDlistByobjID
						(_entity_id, pair_list,	pair_len);
			}
			else
			{
				(this->kvstore)->getpreIDobjIDlistBysubID
						(_entity_id, pair_list,	pair_len);
			}

			bool exist_preid = util::bsearch_preid_uporder
					(pre_id, pair_list,	pair_len);

			if (exist_preid)
			{
				remain_list[remain_len] = _entity_id;
				remain_len++;
			}

			delete[] pair_list;
			pair_len = 0;
		}/* end for i 0 to _list.size */

		_list.intersectList(remain_list, remain_len);

		/* can be imported */
		delete[] remain_list;
	}/* end for j : varDegree */
}

void Database::only_pre_filter_after_join(BasicQuery* basic_query)
{
    int var_num = basic_query->getVarNum();
    vector<int*>& result_list = basic_query->getResultList();

    for (int var_id = 0; var_id < var_num; var_id++)
    {
        int var_degree = basic_query->getVarDegree(var_id);

        // get all the only predicate filter edges for this variable.
        vector<int> in_edge_pre_id;
        vector<int> out_edge_pre_id;
        for (int i = 0; i < var_degree; i++)
        {
            char edge_type = basic_query->getEdgeType(var_id, i);
            int triple_id = basic_query->getEdgeID(var_id, i);
            Triple triple = basic_query->getTriple(triple_id);
            std::string neighbor_name;

            if (edge_type == BasicQuery::EDGE_OUT)
            {
                neighbor_name = triple.object;
            }
            else
            {
                neighbor_name = triple.subject;
            }

            bool only_preid_filter = (basic_query->isOneDegreeNotSelectVar(neighbor_name));
            if (!only_preid_filter)
            {
                continue;
            }

            int pre_id = basic_query->getEdgePreID(var_id, i);

            if (edge_type == BasicQuery::EDGE_OUT)
            {
                out_edge_pre_id.push_back(pre_id);
            }
            else
            {
                in_edge_pre_id.push_back(pre_id);
            }
        }

        if (in_edge_pre_id.empty() && out_edge_pre_id.empty())
        {
            continue;
        }

        for (vector<int*>::iterator itr = result_list.begin(); itr != result_list.end(); itr++)
        {
            int* res_seq = (*itr);
            if (res_seq[var_num] == -1)
            {
                continue;
            }

            int entity_id = res_seq[var_id];
            int* pair_list = NULL;
            int pair_len = 0;
            bool exist_preid = true;

            if (exist_preid && !in_edge_pre_id.empty())
            {
                (this->kvstore)->getpreIDsubIDlistByobjID(entity_id, pair_list, pair_len);

                for (vector<int>::iterator itr_pre = in_edge_pre_id.begin(); itr_pre != in_edge_pre_id.end(); itr_pre++)
                {
                    int pre_id = (*itr_pre);
                    bool exist_preid = util::bsearch_preid_uporder(pre_id, pair_list, pair_len);
                    if (!exist_preid)
                    {
                        break;
                    }
                }
            }
            if (exist_preid && !out_edge_pre_id.empty())
            {
                (this->kvstore)->getpreIDobjIDlistBysubID(entity_id, pair_list, pair_len);

                for (vector<int>::iterator itr_pre = out_edge_pre_id.begin(); itr_pre != out_edge_pre_id.end(); itr_pre++)
                {
                    int pre_id = (*itr_pre);
                    bool exist_preid = util::bsearch_preid_uporder(pre_id, pair_list, pair_len);
                    if (!exist_preid)
                    {
                        break;
                    }
                }
            }
            delete []pair_list;

            // result sequence is illegal when there exists any missing filter predicate id.
            if (!exist_preid)
            {
                res_seq[var_num] = -1;
            }
        }
    }
}

/* add literal candidates to these variables' candidate list which may include literal results. */
void Database::add_literal_candidate(BasicQuery* basic_query)
{
    Database::log("IN add_literal_candidate");

    int var_num = basic_query->getVarNum();

    // deal with literal variable candidate list.
    // because we do not insert any literal elements into VSTree, we can not retrieve them from VSTree.
    // for these variable which may include some literal results, we should add all possible literal candidates to the candidate list.
    for (int i = 0; i < var_num; i++)
    {
        //debug
        {
            stringstream _ss;
            _ss << "var[" << i << "]\t";
            if (basic_query->isLiteralVariable(i))
            {
                _ss << "may have literal result.";
            }
            else
            {
                _ss << "do not have literal result.";
            }
            _ss << endl;
            Database::log(_ss.str());
        }

        if (!basic_query->isLiteralVariable(i))
        {
            // if this variable is not literal variable, we can assume that its literal candidates have been added.
            basic_query->setAddedLiteralCandidate(i);
            continue;
        }

        // for these literal variable without any linking entities(we call free literal variable),
        // we will add their literal candidates when join-step.
        if (basic_query->isFreeLiteralVariable(i))
        {
            continue;
        }


        int var_id = i;
        int var_degree = basic_query->getVarDegree(var_id);
        IDList literal_candidate_list;

        // intersect each edge's literal candidate.
        for (int j = 0; j < var_degree; j ++)
        {
            int neighbor_id = basic_query->getEdgeNeighborID(var_id, j);
            int predicate_id = basic_query->getEdgePreID(var_id, j);
            int triple_id = basic_query->getEdgeID(var_id, j);
            Triple triple = basic_query->getTriple(triple_id);
            std::string neighbor_name = triple.subject;
            IDList this_edge_literal_list;

            // if the neighbor of this edge is an entity, we can add all literals which has an exact predicate edge linking to this entity.
            if (neighbor_id == -1)
            {
                int subject_id = (this->kvstore)->getIDByEntity(neighbor_name);
                int* object_list = NULL;
                int object_list_len = 0;

                (this->kvstore)->getobjIDlistBysubIDpreID(subject_id, predicate_id, object_list, object_list_len);
                this_edge_literal_list.unionList(object_list, object_list_len);
                delete []object_list;
            }
            // if the neighbor of this edge is variable, then the neighbor variable can not have any literal results,
            // we should add literals when join these two variables, see the Database::join function for details.

            // deprecated...
            // if the neighbor of this edge is variable, we should add all this neighbor variable's candidate entities' neighbor literal,
            // which has one corresponding predicate edge linking to this variable.
            else
            {

                /*
                IDList& neighbor_candidate_list = basic_query->getCandidateList(neighbor_id);
                int neighbor_candidate_list_size = neighbor_candidate_list.size();
                for (int k = 0;k < neighbor_candidate_list_size; k ++)
                {
                    int subject_id = neighbor_candidate_list.getID(k);
                    int* object_list = NULL;
                    int object_list_len = 0;

                    (this->kvstore)->getobjIDlistBysubIDpreID(subject_id, predicate_id, object_list, object_list_len);
                    this_edge_literal_list.unionList(object_list, object_list_len);
                    delete []object_list;
                }
                */
            }


            if (j == 0)
            {
                literal_candidate_list.unionList(this_edge_literal_list);
            }
            else
            {
                literal_candidate_list.intersectList(this_edge_literal_list);
            }
        }

        // add the literal_candidate_list to the original candidate list.
        IDList& origin_candidate_list = basic_query->getCandidateList(var_id);
        int origin_candidate_list_len = origin_candidate_list.size();
        origin_candidate_list.unionList(literal_candidate_list);
        int after_add_literal_candidate_list_len = origin_candidate_list.size();

        // this variable's literal candidates have been added.
        basic_query->setAddedLiteralCandidate(var_id);

        //debug
        {
            stringstream _ss;
            _ss << "var[" << var_id << "] candidate list after add literal:\t"
                << origin_candidate_list_len << "-->" << after_add_literal_candidate_list_len << endl;
            /*
            for (int i = 0; i < after_add_literal_candidate_list_len; i ++)
            {
                int candidate_id = origin_candidate_list.getID(i);
                std::string candidate_name;
                if (i < origin_candidate_list_len)
                {
                    candidate_name = (this->kvstore)->getEntityByID(origin_candidate_list.getID(i));
                }
                else
                {
                    candidate_name = (this->kvstore)->getLiteralByID(origin_candidate_list.getID(i));
                }
                _ss << candidate_name << "(" << candidate_id << ")\t";
            }
            */
            Database::log(_ss.str());
        }
    }

    Database::log("OUT add_literal_candidate");
}

/* get the final string result_set from SPARQLquery */
bool Database::getFinalResult(SPARQLquery& _sparql_q, ResultSet& _result_set)
{
	int _var_num = _sparql_q.getQueryVarNum();
	_result_set.setVar(_sparql_q.getQueryVar());
	std::vector<BasicQuery*>& query_vec = _sparql_q.getBasicQueryVec();

	/* sum the answer number */
	int _ans_num = 0;
	for(int i = 0; i < query_vec.size(); i ++)
	{
		_ans_num += query_vec[i]->getResultList().size();
	}

	_result_set.ansNum = _ans_num;
	_result_set.answer = new string*[_ans_num];
	for(int i = 0; i < _result_set.ansNum; i ++)
	{
		_result_set.answer[i] = NULL;
	}

	int tmp_ans_count = 0;
	/* map int ans into string ans
	 * union every basic result into total result
	 *  */
	for(int i = 0; i < query_vec.size(); i ++)
	{
		std::vector<int*>& tmp_vec = query_vec[i]->getResultList();
		std::vector<int*>::iterator itr = tmp_vec.begin();

		/* for every result group in resultlist */
		for(; itr != tmp_vec.end(); itr ++)
		{
			_result_set.answer[tmp_ans_count] = new string[_var_num];
			/* map every ans_id into ans_str */
			for(int v = 0; v < _var_num; v ++)
			{
				int ans_id = (*itr)[v];
				string ans_str;
				if (this->objIDIsEntityID(ans_id))
				{
				    ans_str = (this->kvstore)->getEntityByID(ans_id);

				}
				else
				{
				    ans_str = (this->kvstore)->getLiteralByID(ans_id);
				}
				_result_set.answer[tmp_ans_count][v] = ans_str;
			}
			tmp_ans_count ++;
		}
	}

	return true;
}


FILE* Database::fp_debug = NULL;
void Database::log(std::string _str)
{
	_str += "\n";
	if(Database::debug_1)
	{
		fputs(_str.c_str(), fp_debug);
		fflush(fp_debug);
	}

	if(Database::debug_vstree)
	{
		fputs(_str.c_str(), fp_debug);
		fflush(fp_debug);

	}
}

void Database::printIDlist(int _i, int* _list, int _len, std::string _log)
{
	std::stringstream _ss;
	_ss << "[" << _i << "] ";
	for(int i = 0; i < _len; i ++){
		_ss << _list[i] << "\t";
	}
	Database::log("=="+_log + ":");
	Database::log(_ss.str());
}
void Database::printPairList(int _i, int* _list, int _len, std::string _log)
{
	std::stringstream _ss;
	_ss << "[" << _i << "] ";
	for(int i = 0; i < _len; i += 2){
		_ss << "[" << _list[i] << "," << _list[i+1] << "]\t";
	}
	Database::log("=="+_log + ":");
	Database::log(_ss.str());
}

void Database::test()
 {
	int subNum = 9, preNum = 20, objNum = 90;

	int* _id_list = NULL;
	int _list_len = 0;
	{/* x2ylist */
		for (int i = 0; i < subNum; i++) {

			(this->kvstore)->getobjIDlistBysubID(i, _id_list, _list_len);
			if (_list_len != 0) {
				stringstream _ss;
				this->printIDlist(i, _id_list, _list_len, "s2olist["+_ss.str()+"]");
				delete[] _id_list;
			}

			/* o2slist */
			(this->kvstore)->getsubIDlistByobjID(i, _id_list, _list_len);
			if (_list_len != 0) {
				stringstream _ss;
				this->printIDlist(i, _id_list, _list_len, "o(sub)2slist["+_ss.str()+"]");
				delete[] _id_list;
			}
		}

		for (int i = 0; i < objNum; i++) {
			int _i = Database::LITERAL_FIRST_ID + i;
			(this->kvstore)->getsubIDlistByobjID(_i, _id_list, _list_len);
			if (_list_len != 0) {
				stringstream _ss;
				this->printIDlist(_i, _id_list, _list_len, "o(literal)2slist["+_ss.str()+"]");
				delete[] _id_list;
			}
		}
	}
	{/* xy2zlist */
		for (int i = 0; i < subNum; i++) {
			for (int j = 0; j < preNum; j++) {
				(this->kvstore)->getobjIDlistBysubIDpreID(i, j, _id_list,
						_list_len);
				if (_list_len != 0) {
					stringstream _ss;
					_ss << "preid:" << j ;
					this->printIDlist(i, _id_list, _list_len, "sp2olist["+_ss.str()+"]");
					delete[] _id_list;
				}

				(this->kvstore)->getsubIDlistByobjIDpreID(i, j, _id_list,
						_list_len);
				if (_list_len != 0) {
					stringstream _ss;
					_ss << "preid:" << j ;
					this->printIDlist(i, _id_list, _list_len, "o(sub)p2slist["+_ss.str()+"]");
					delete[] _id_list;
				}
			}
		}

		for (int i = 0; i < objNum; i++) {
			int _i = Database::LITERAL_FIRST_ID + i;
			for (int j = 0; j < preNum; j++) {
				(this->kvstore)->getsubIDlistByobjIDpreID(_i, j, _id_list,
						_list_len);
				if (_list_len != 0) {
					stringstream _ss;
					_ss << "preid:" << j ;
					this->printIDlist(_i, _id_list, _list_len,
							"*o(literal)p2slist["+_ss.str()+"]");
					delete[] _id_list;
				}
			}
		}
	}
	{/* x2yzlist */
		for (int i = 0; i < subNum; i++) {
			(this->kvstore)->getpreIDobjIDlistBysubID(i, _id_list, _list_len);
			if (_list_len != 0) {
				this->printPairList(i, _id_list, _list_len, "s2polist");
				delete[] _id_list;
				_list_len = 0;
			}
		}

		for (int i = 0; i < subNum; i++) {
			(this->kvstore)->getpreIDsubIDlistByobjID(i, _id_list, _list_len);
			if (_list_len != 0) {
				this->printPairList(i, _id_list, _list_len, "o(sub)2pslist");
				delete[] _id_list;
			}
		}

		for (int i = 0; i < objNum; i++) {
			int _i = Database::LITERAL_FIRST_ID + i;
			(this->kvstore)->getpreIDsubIDlistByobjID(_i, _id_list, _list_len);
			if (_list_len != 0) {
				this->printPairList(_i, _id_list, _list_len,
						"o(literal)2pslist");
				delete[] _id_list;
			}
		}
	}
}

void Database::test_build_sig()
{
	BasicQuery* _bq = new BasicQuery("");
	/*
	 * <!!!>	y:created	<!!!_(album)>.
	 *  <!!!>	y:created	<Louden_Up_Now>.
	 *  <!!!_(album)>	y:hasSuccessor	<Louden_Up_Now>
	 * <!!!_(album)>	rdf:type	<wordnet_album_106591815>
	 *
	 * id of <!!!> is 0
	 * id of <!!!_(album)> is 2
	 *
	 *
	 * ?x1	y:created	?x2.
	 *  ?x1	y:created	<Louden_Up_Now>.
	 *  ?x2	y:hasSuccessor	<Louden_Up_Now>.
	 * ?x2	rdf:type	<wordnet_album_106591815>
	 */
	{
		Triple _triple("?x1", "y:created", "?x2");
		_bq->addTriple(_triple);
	}
	{
		Triple _triple("?x1", "y:created", "<Louden_Up_Now>");
		_bq->addTriple(_triple);
	}
	{
		Triple _triple("?x2", "y:hasSuccessor", "<Louden_Up_Now>");
		_bq->addTriple(_triple);
	}
	{
		Triple _triple("?x2", "rdf:type", "<wordnet_album_106591815>");
		_bq->addTriple(_triple);
	}
	std::vector<std::string> _v;
	_v.push_back("?x1");
	_v.push_back("?x2");

	_bq->encodeBasicQuery(this->kvstore, _v);
	Database::log(_bq->to_str());
	SPARQLquery _q;
	_q.addBasicQuery(_bq);

	(this->vstree)->retrieve(_q);

	Database::log("\n\n");
	Database::log("candidate:\n\n"+_q.candidate_str());
}

void Database::test_join()
{
	BasicQuery* _bq = new BasicQuery("");
	/*
	 * <!!!>	y:created	<!!!_(album)>.
	 *  <!!!>	y:created	<Louden_Up_Now>.
	 *  <!!!_(album)>	y:hasSuccessor	<Louden_Up_Now>
	 * <!!!_(album)>	rdf:type	<wordnet_album_106591815>
	 *
	 * id of <!!!> is 0
	 * id of <!!!_(album)> is 2
	 *
	 *
	 * ?x1	y:created	?x2.
	 *  ?x1	y:created	<Louden_Up_Now>.
	 *  ?x2	y:hasSuccessor	<Louden_Up_Now>.
	 * ?x2	rdf:type	<wordnet_album_106591815>
	 */
	{
		Triple _triple("?x1", "y:created", "?x2");
		_bq->addTriple(_triple);
	}
	{
		Triple _triple("?x1", "y:created", "<Louden_Up_Now>");
		_bq->addTriple(_triple);
	}
	{
		Triple _triple("?x2", "y:hasSuccessor", "<Louden_Up_Now>");
		_bq->addTriple(_triple);
	}
	{
		Triple _triple("?x2", "rdf:type", "<wordnet_album_106591815>");
		_bq->addTriple(_triple);
	}
	std::vector<std::string> _v;
	_v.push_back("?x1");
	_v.push_back("?x2");

	_bq->encodeBasicQuery(this->kvstore, _v);
	Database::log(_bq->to_str());
	SPARQLquery _q;
	_q.addBasicQuery(_bq);

	(this->vstree)->retrieve(_q);

	Database::log("\n\n");
	Database::log("candidate:\n\n"+_q.candidate_str());
	_q.print(std::cout);

	this->join(_q);
	ResultSet _rs;
	this->getFinalResult(_q, _rs);
	cout << _rs.to_str() << endl;
}