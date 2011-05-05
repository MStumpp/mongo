/**
*    Copyright (C) 2011 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"

#include "db/pipeline/document_source.h"

#include "db/jsobj.h"
#include "db/pipeline/accumulator.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"

namespace mongo {
    const char DocumentSourceGroup::groupName[] = "$group";

    string DocumentSourceGroup::idName("_id");

    DocumentSourceGroup::~DocumentSourceGroup() {
    }

    bool DocumentSourceGroup::eof() {
        if (!populated)
            populate();

        return (groupsIterator == groups.end());
    }

    bool DocumentSourceGroup::advance() {
        if (!populated)
            populate();

        assert(groupsIterator != groups.end()); // CW TODO error

        ++groupsIterator;
        if (groupsIterator == groups.end()) {
            pCurrent.reset();
            return false;
        }

        pCurrent = makeDocument(groupsIterator);
        return true;
    }

    boost::shared_ptr<Document> DocumentSourceGroup::getCurrent() {
        if (!populated)
            populate();

        return pCurrent;
    }

    void DocumentSourceGroup::sourceToBson(BSONObjBuilder *pBuilder) const {
	BSONObjBuilder insides;

	/* add the _id */
	pIdExpression->addToBsonObj(&insides, "_id", false);

	/* add the remaining fields */
	const size_t n = vFieldName.size();
	for(size_t i = 0; i < n; ++i) {
	    boost::shared_ptr<Accumulator> pA((*vpAccumulatorFactory[i])(pCtx));
	    pA->addOperand(vpExpression[i]);
	    pA->addToBsonObj(&insides, vFieldName[i], true);
	}

	pBuilder->append("$group", insides.done());
    }

    boost::shared_ptr<DocumentSourceGroup> DocumentSourceGroup::create(
	const intrusive_ptr<ExpressionContext> &pCtx) {
        boost::shared_ptr<DocumentSourceGroup> pSource(
            new DocumentSourceGroup(pCtx));
        return pSource;
    }

    DocumentSourceGroup::DocumentSourceGroup(
	const intrusive_ptr<ExpressionContext> &pTheCtx):
        populated(false),
        pIdExpression(),
        groups(),
        vFieldName(),
        vpAccumulatorFactory(),
        vpExpression(),
        pCtx(pTheCtx) {
    }

    void DocumentSourceGroup::addAccumulator(
        string fieldName,
        boost::shared_ptr<Accumulator> (*pAccumulatorFactory)(
	    const intrusive_ptr<ExpressionContext> &),
        boost::shared_ptr<Expression> pExpression) {
        vFieldName.push_back(fieldName);
        vpAccumulatorFactory.push_back(pAccumulatorFactory);
        vpExpression.push_back(pExpression);
    }


    struct GroupOpDesc {
        const char *pName;
        boost::shared_ptr<Accumulator> (*pFactory)(
	    const intrusive_ptr<ExpressionContext> &);
    };

    static int GroupOpDescCmp(const void *pL, const void *pR) {
        return strcmp(((const GroupOpDesc *)pL)->pName,
                      ((const GroupOpDesc *)pR)->pName);
    }

    /*
      Keep these sorted alphabetically so we can bsearch() them using
      GroupOpDescCmp() above.
    */
    static const GroupOpDesc GroupOpTable[] = {
        {"$avg", AccumulatorAvg::create},
        {"$max", AccumulatorMinMax::createMax},
        {"$min", AccumulatorMinMax::createMin},
        {"$push", AccumulatorPush::create},
        {"$sum", AccumulatorSum::create},
    };

    static const size_t NGroupOp = sizeof(GroupOpTable)/sizeof(GroupOpTable[0]);

    boost::shared_ptr<DocumentSource> DocumentSourceGroup::createFromBson(
	BSONElement *pBsonElement,
	const intrusive_ptr<ExpressionContext> &pCtx) {
        assert(pBsonElement->type() == Object); // CW TODO must be an object

        boost::shared_ptr<DocumentSourceGroup> pGroup(
	    DocumentSourceGroup::create(pCtx));
        bool idSet = false;

        BSONObj groupObj(pBsonElement->Obj());
        BSONObjIterator groupIterator(groupObj);
        while(groupIterator.more()) {
            BSONElement groupField(groupIterator.next());
            const char *pFieldName = groupField.fieldName();

            if (strcmp(pFieldName, "_id") == 0) {
                assert(!idSet); // CW TODO _id specified multiple times
                assert(groupField.type() == Object); // CW TODO error message

                /*
                  Use the projection-like set of field paths to create the
                  group-by key.
                 */
                boost::shared_ptr<Expression> pId(
                    Expression::parseObject(&groupField,
				 &Expression::ObjectCtx(
					Expression::ObjectCtx::DOCUMENT_OK)));
                pGroup->setIdExpression(pId);
                idSet = true;
            }
            else {
                /*
                  Treat as a projection field with the additional ability to
                  add aggregation operators.
                */
                assert(*pFieldName != '$');
                // CW TODO error: field name can't be an operator
                assert(groupField.type() == Object);
                // CW TODO error: must be an operator expression

                BSONObj subField(groupField.Obj());
                BSONObjIterator subIterator(subField);
                size_t subCount = 0;
                for(; subIterator.more(); ++subCount) {
                    BSONElement subElement(subIterator.next());

                    /* look for the specified operator */
                    GroupOpDesc key;
                    key.pName = subElement.fieldName();
                    const GroupOpDesc *pOp =
			(const GroupOpDesc *)bsearch(
                              &key, GroupOpTable, NGroupOp, sizeof(GroupOpDesc),
                                      GroupOpDescCmp);

                    assert(pOp); // CW TODO error: operator not found

                    boost::shared_ptr<Expression> pGroupExpr;

                    BSONType elementType = subElement.type();
                    if (elementType == Object)
                        pGroupExpr = Expression::parseObject(&subElement,
                                        &Expression::ObjectCtx(
				     Expression::ObjectCtx::DOCUMENT_OK));
                    else if (elementType == Array) {
                        assert(false); // CW TODO group operators are unary
                    }
                    else { /* assume its an atomic single operand */
                        pGroupExpr = Expression::parseOperand(&subElement);
                    }

                    pGroup->addAccumulator(
                        pFieldName, pOp->pFactory, pGroupExpr);
                }

                assert(subCount == 1);
                // CW TODO error: only one operator allowed
            }
        }

        assert(idSet); // CW TODO error: missing _id specification

        return pGroup;
    }

    void DocumentSourceGroup::populate() {
        for(bool hasNext = !pSource->eof(); hasNext;
                hasNext = pSource->advance()) {
            boost::shared_ptr<Document> pDocument(pSource->getCurrent());

            /* get the _id document */
            boost::shared_ptr<const Value> pId(pIdExpression->evaluate(pDocument));

            /*
              Look for the _id value in the map; if it's not there, add a
               new entry with a blank accumulator.
            */
            vector<boost::shared_ptr<Accumulator> > *pGroup;
            GroupsType::iterator it(groups.find(pId));
            if (it != groups.end()) {
                /* point at the existing accumulators */
                pGroup = &it->second;
            }
            else {
                /* insert a new group into the map */
                groups.insert(it,
                              pair<boost::shared_ptr<const Value>,
                              vector<boost::shared_ptr<Accumulator> > >(
                                  pId, vector<boost::shared_ptr<Accumulator> >()));

                /* find the accumulator vector (the map value) */
                it = groups.find(pId);
                pGroup = &it->second;

                /* add the accumulators */
                const size_t n = vpAccumulatorFactory.size();
                pGroup->reserve(n);
                for(size_t i = 0; i < n; ++i) {
                    boost::shared_ptr<Accumulator> pAccumulator(
                        (*vpAccumulatorFactory[i])(pCtx));
                    pAccumulator->addOperand(vpExpression[i]);
                    pGroup->push_back(pAccumulator);
                }
            }

            /* point at the existing key */
            // unneeded atm // pId = it.first;

            /* tickle all the accumulators for the group we found */
            const size_t n = pGroup->size();
            for(size_t i = 0; i < n; ++i)
                (*pGroup)[i]->evaluate(pDocument);
        }

        /* start the group iterator */
        groupsIterator = groups.begin();
        if (groupsIterator != groups.end())
            pCurrent = makeDocument(groupsIterator);
        populated = true;
    }

    boost::shared_ptr<Document> DocumentSourceGroup::makeDocument(
        const GroupsType::iterator &rIter) {
        vector<boost::shared_ptr<Accumulator> > *pGroup = &rIter->second;
        const size_t n = vFieldName.size();
        boost::shared_ptr<Document> pResult(Document::create(1 + n));

        /* add the _id field */
        pResult->addField(idName, rIter->first);

        /* add the rest of the fields */
        for(size_t i = 0; i < n; ++i)
            pResult->addField(vFieldName[i], (*pGroup)[i]->getValue());

        return pResult;
    }

    boost::shared_ptr<DocumentSource> DocumentSourceGroup::createMerger() {
	boost::shared_ptr<DocumentSourceGroup> pMerger(
	    DocumentSourceGroup::create(pCtx));

	/* the merger will use the same grouping key */
	pMerger->setIdExpression(ExpressionFieldPath::create("_id"));

	const size_t n = vFieldName.size();
	for(size_t i = 0; i < n; ++i) {
	    /*
	      The merger's output field names will be the same, as will the
	      accumulator factories.  However, for some accumulators, the
	      expression to be accumulated will be different.  The original
	      accumulator may be collecting an expression based on a field
	      expression or constant.  Here, we accumulate the output of the
	      same name from the prior group.
	    */
	    pMerger->addAccumulator(
		vFieldName[i], vpAccumulatorFactory[i],
		ExpressionFieldPath::create(vFieldName[i]));
	}

	return pMerger;
    }
}

