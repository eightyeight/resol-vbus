/**
 * @license
 * resol-vbus - A JavaScript library for processing RESOL VBus data
 */
'use strict';



var _ = require('lodash');


var Packet = require('./resol-vbus').Packet;


var connectionSpec = require('./connection.spec');



describe('Packet', function() {

    it('should be a constructor function', function() {
        expect(Packet).to.be.a('function');
        expect(Packet.extend).to.be.a('function');
    });

    it('should have reasonable defaults', function() {
        var before = new Date();
        var packet = new Packet();
        var after = new Date();

        expect(packet).to.be.an('object');
        expect(packet.timestamp).to.be.an.instanceOf(Date);
        expect(packet.timestamp.getTime()).to.be.within(before.getTime(), after.getTime());
        expect(packet.channel).to.eql(0);
        expect(packet.destinationAddress).to.eql(0);
        expect(packet.sourceAddress).to.eql(0);
        expect(packet.command).to.eql(0);
        expect(packet.frameCount).to.eql(0);
        expect(packet.frameData).to.be.an.instanceOf(Buffer);
        expect(packet.frameData.length).to.eql(127 * 4);
    });

    it('should copy certain options', function() {
        var frameData = new Buffer(13 * 4);
        for (var i = 0; i < frameData.length; i++) {
            frameData [i] = i * 4;
        }

        var options = {
            timestamp: new Date(0),
            channel: 0x1337,
            destinationAddress: 0x2336,
            sourceAddress: 0x3335,
            command: 0x4334,
            frameCount: 13,
            frameData: frameData,
            junk: 0x7331
        };

        var packet = new Packet(options);

        expect(packet).to.be.an('object');
        expect(packet.timestamp).to.eql(options.timestamp);
        expect(packet.channel).to.eql(options.channel);
        expect(packet.destinationAddress).to.eql(options.destinationAddress);
        expect(packet.sourceAddress).to.eql(options.sourceAddress);
        expect(packet.command).to.eql(options.command);
        expect(packet.frameCount).to.eql(options.frameCount);
        expect(packet.frameData).to.be.an.instanceOf(Buffer);
        expect(packet.frameData.slice(0, frameData.length)).to.eql(frameData);
        expect(packet.junk).to.eql(undefined);
    });

    it('should have a toBuffer method', function() {
        var frameData = new Buffer(13 * 4);
        for (var i = 0; i < frameData.length; i++) {
            frameData [i] = i * 4;
        }

        var options = {
            destinationAddress: 0x2336,
            sourceAddress: 0x3335,
            command: 0x4334,
            frameCount: 13,
            frameData: frameData,
        };

        var packet = new Packet(options);

        var buffer = packet.toBuffer();

        expect(buffer).to.be.an.instanceOf(Buffer);
        expect(buffer.length).to.eql(88);
        expect(buffer.toString('hex')).to.eql('aa362335331034430d2a0004080c00671014181c00272024282c00673034383c00274044484c00675054585c00276064686c00677074787c00270004080c0f581014181c0f182024282c0f583034383c0f184044484c0f58');

/*
        var stats = connectionSpec.parseRawDataOnce(buffer);
        expect(stats.rawDataCount).to.eql(buffer.length);
        expect(stats.junkDataCount).to.eql(0);
        expect(stats.packetCount).to.eql(1);
        expect(stats.lastPacket).to.be.an.instanceOf(Packet);

        _.forEach(options, function(optionsValue, key) {
            var value = stats.lastPacket [key];
            var refValue = packet [key];

            if (value instanceof Buffer) {
                value = value.toString('hex');
            }
            if (refValue instanceof Buffer) {
                refValue = refValue.toString('hex');
            }

            expect(value).to.equal(refValue, key);
        });
*/
    });

    it('should have a fromBuffer function', function() {
        var frameData = new Buffer(13 * 4);
        for (var i = 0; i < frameData.length; i++) {
            frameData [i] = i * 4;
        }

        var options = {
            destinationAddress: 0x2336,
            sourceAddress: 0x3335,
            command: 0x4334,
            frameCount: 13,
            frameData: frameData,
        };

        var buffer = new Buffer('aa362335331034430d2a0004080c00671014181c00272024282c00673034383c00274044484c00675054585c00276064686c00677074787c00270004080c0f581014181c0f182024282c0f583034383c0f184044484c0f58', 'hex');

        var packet = Packet.fromBuffer(buffer, 0, buffer.length);

        expect(packet).to.be.an.instanceOf(Packet);

        _.forEach(options, function(refValue, key) {
            var value = packet [key];

            if ((value instanceof Buffer) && (refValue instanceof Buffer)) {
                value = value.slice(0, refValue.length).toString('hex');
                refValue = refValue.toString('hex');
            }

            expect(value).to.equal(refValue, key);
        });
    });

});