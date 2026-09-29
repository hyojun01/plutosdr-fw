// SPDX-License-Identifier: GPL-2.0-or-later
// Input/output observed in the same clock interval before next rising edge:
// one register = one sample. Production generated RTL is not modified.
`timescale 1ns/1ns
module tb_latency;
reg clk=0, reset=1, load=0;
reg signed [15:0] sample=0;
reg [9:0] id=0;
reg [7:0] fd=0;
reg [24:0] scale=25'd65536;
integer selected=1;
wire signed [15:0] out_i, out_q;
always #5 clk=~clk;
HDL_DUT_ip_src_HDL_DUT dut(
  .clk(clk),
  .reset(reset),
  .clk_enable(1'b1),
  .Rx_Data_I_In(sample),
  .Rx_Data_Q_In(16'sd0),
  .Rx_Valid_In(1'b1),
  .ID1(id),
  .ID2(id),
  .ID3(id),
  .ID4(id),
  .FD1(fd),
  .FD2(fd),
  .FD3(fd),
  .FD4(fd),
  .FRQ1(32'd0),
  .FRQ2(32'd0),
  .FRQ3(32'd0),
  .FRQ4(32'd0),
  .PHOF1(32'd0),
  .PHOF2(32'd0),
  .PHOF3(32'd0),
  .PHOF4(32'd0),
  .SC1(scale),
  .SC2(scale),
  .SC3(scale),
  .SC4(scale),
  .EN1((selected == 1 || selected == 0)),
  .EN2((selected == 2 || selected == 0)),
  .EN3((selected == 3 || selected == 0)),
  .EN4((selected == 4 || selected == 0)),
  .LP1(load),
  .LP2(load),
  .LP3(load),
  .LP4(load),
  .ce_out(),
  .Tx_Data_I_Out(out_i),
  .Tx_Data_Q_Out(out_q),
  .Tx_Valid_Out());
integer k, n, file;
initial begin
 file=$fopen("impulse.csv", "w");
 $fdisplay(file,"case,target,id,fd,scale,n,i,q");
 for (k=0;k<12;k=k+1) begin
  reset=1; sample=0; load=0;
  repeat(4) @(negedge clk);
  reset=0;
  id=(k==1 ? 16 : k==2 ? 1023 : 0);
  fd=(k==3 ? 32 : k==4 ? 63 : 0);
  selected=(k==5 ? 2 : k==6 ? 3 : k==7 ? 4 : k==8 || k==9 ? 0 : k==11 ? 5 : 1);
  scale=(k==9 ? 25'd16384 : k==10 ? 25'd32768 : 25'd65536);
  load=1;
  repeat(4) @(negedge clk);
  load=0;
  repeat(1200) @(negedge clk);
  for(n=0;n<1150;n=n+1) begin
   sample=(n==0 ? 16'sd16384 : 16'sd0);
   #1;
   $fdisplay(file,"%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d",k,selected,id,fd,scale,n,out_i,out_q);
   @(negedge clk);
  end
 end
 $fclose(file);
 $display("RTL impulse captures complete");
 $finish;
end
endmodule
