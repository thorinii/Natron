//  Powiter
//
//  Created by Alexandre Gauthier-Foichat on 06/12
//  Copyright (c) 2013 Alexandre Gauthier-Foichat. All rights reserved.
//  contact: immarespond at gmail dot com
#ifndef OUTPUTNODE_H
#define OUTPUTNODE_H
#include "Core/node.h"


class OutputNode : public Node
{
public:

    OutputNode();
   
    virtual ~OutputNode(){}
    
    virtual bool isOutputNode(){return true;}
    virtual int maximumSocketCount(){return 0;}
   
    virtual int maximumInputs(){return 1;}
    virtual int minimumInputs(){return 1;}
    virtual bool cacheData(){return false;}


};


#endif // OUTPUTNODE_H
