// This file tests the mapping of SystemVerilog expressions onto
// the internal Yosys cell library.
//
// Each expression passed in argument to `$t` is evaluated twice:
// The result of slang's constant evaluation is checked to agree
// with the result obtained by evaluating the mapped Yosys cells.
//
// Nets and static variables cannot be used; but functions, automatic
// variables, and localparams can be.

module top;

function [7:0] f1();
	f1 = 8'hab;
endfunction;

initial $t(f1());

// unary
initial begin
	$t(-7);
	$t(-4'hf);
	$t(-4'he);
	$t(!(-4));
	$t(!0);
	$t(|3'd0);
	$t(|3'd1);
	$t(|3'd3);
	$t(&3'd0);
	$t(&3'd6);
	$t(&3'd7);
	$t(~&3'd0);
	$t(~&3'd6);
	$t(~&3'd7);
	$t(~|3'd0);
	$t(~|3'd1);
	$t(~|3'd3);
	$t(~^3'd0);
	$t(~^3'd1);
	$t(~^3'd3);
end

// binary
initial begin
	$t(-1 + -1);
	$t(-8'd1 + 1);
	$t(8'shff + $unsigned(1));
	$t((8'd5 - 8'd8) + 16'd1);
	$t(3'd1 * 8'd7);
	$t(13 / 4);
	$t(-13 / 4);
	$t(13 / -4);
	$t(-13 / -4);
	$t(13 % 4);
	$t(-13 % 4);
	$t(13 % -4);
	$t(-13 % -4);
	$t(16'd123 & 16'd71);
	$t(16'd123 | 16'd71);
	$t(16'd123 ^ 16'd71);
	$t(16'd123 ~^ 16'd71);
	$t(4'hf > -11);
	$t(-1 > 7);
	$t(3 > 3);
	$t(4'hf >= -11);
	$t(-1 >= 7);
	$t(3 >= 3);
	$t(4'hf < -11);
	$t(-1 < 7);
	$t(3 < 3);
	$t(4'hf <= -11);
	$t(-1 <= 7);
	$t(3 <= 3);
	$t(2 -> 1);
	$t(0 -> 3);
	$t(-1 -> 0);
	$t(0 -> 0);
	$t(2 <-> 1);
	$t(0 <-> 3);
	$t(-1 <-> 0);
	$t(0 <-> 0);
	$t(2 <-> 1);
	$t(0 <-> 3);
	$t(-1 <-> 0);
	$t(8'd1 << 5);
	$t(-8'h2 << 3);
	$t(8'd1 >> 5);
	$t(-8'd2 >> 3);
	$t(8'd1 <<< 5);
	$t(-8'd2 <<< 3);
	$t(8'd1 >>> 5);
	$t(-8'd2 >>> 3);
	$t(2'd1 == 8'd2);
	$t(-2'd1 == 8'hff);
	$t(3**2);
	$t(-3**2);
	$t(-3**-2);
	$t(-3**-7);
	$t(-3**0);
	$t(0**7);
end

typedef logic [7:0] byte_t;

initial begin
	$t(byte_t'({<<1{8'hd6}}));
	$t(byte_t'({>>1{8'hd6}}));
	$t(byte_t'({<<2{8'hd6}}));
	$t(byte_t'({>>2{8'hd6}}));
	$t(byte_t'({>>3{8'hd6}}));
	$t(byte_t'({<<3{8'hd6}}));
	$t(byte_t'({>>5{8'hd6}}));
	$t(byte_t'({<<5{8'hd6}}));
end

function [7:0] stream1();
	stream1 = {<<1{8'hd6}};
endfunction
function [7:0] stream2();
	stream2 = {>>1{7'h56}};
endfunction
function [7:0] stream3();
	stream3 = {<<2{8'hd6}};
endfunction
function [7:0] stream4();
	stream4 = {>>2{8'hd6}};
endfunction

initial begin
	$t(stream1());
	$t(stream2());
	$t(stream3());
	$t(stream4());
end

function [7:0] stream5();
	{<<1{stream5}} = 8'hd6;
endfunction
function [7:0] stream6();
	{<<1{stream6}} = 8'hd6;
endfunction
function [7:0] stream7();
	{<<1{stream7}} = 8'hd6;
endfunction
function [7:0] stream8();
	{<<1{stream8}} = 8'hd6;
endfunction

initial begin
	$t(stream5());
	$t(stream6());
	$t(stream7());
	$t(stream8());
end

endmodule
