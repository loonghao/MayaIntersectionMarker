/**
    Copyright (c) 2023 Takayoshi Matsumoto
    You may use, distribute, or modify this code under the terms of the MIT license.
*/
#ifndef UTILITY_H
#define UTILITY_H


#include <vector>
#include <unordered_map>

#include <maya/MGlobal.h>
#include <maya/MItMeshVertex.h>
#include <maya/MIntArray.h>


class PolyChecksum
{
public:
                        PolyChecksum();
    virtual void        putBytes(void* bytes, size_t dataSize);
    virtual int         getResult();

public:
	unsigned long       _table[256];
	unsigned long       _register = 0;
	unsigned long       _key = 0x04c11db7;
};


#endif 


PolyChecksum::PolyChecksum()
{
	// for all possible byte values
	for (unsigned i = 0; i < 256; ++i)
	{
		unsigned long reg = i << 24;
		// for all bits in a byte
		for (int j = 0; j < 8; ++j)
		{
			bool topBit = (reg & 0x80000000) != 0;
			reg <<= 1;

			if (topBit)
				reg ^= _key;
		}
		_table [i] = reg;
	}
}

void PolyChecksum::putBytes(void* bytes, size_t dataSize)
{
    unsigned char* ptr = (unsigned char*) bytes;

    for (size_t i = 0; i < dataSize; i++)
    {
        unsigned byte = *(ptr + i);
        unsigned top = _register >> 24;
        top ^= byte;
        top &= 255;

        _register = (_register << 8) ^ _table [top];
    }
}


int PolyChecksum::getResult()
{
    return (int) this->_register;
}


int getVertexChecksum(MObject polyObject)
{
    PolyChecksum checksum;

    MItMeshVertex itVertex(polyObject);
    
    while (!itVertex.isDone())
    {
        int index = itVertex.index();
        checksum.putBytes(&index, sizeof(index));

        MIntArray connectedVertices;
        itVertex.getConnectedVertices(connectedVertices);
        uint numConnectedVertices = connectedVertices.length();

        for (uint i = 0; i < numConnectedVertices; i++)
        {
            uint idx = connectedVertices[i];
            checksum.putBytes(&idx, sizeof(idx));
        }

        itVertex.next();
    }

    return checksum.getResult();
}