
//////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////
`include "includes.v"

`define CPL_MEM_RD64_FMT_TYPE 7'b10_01010
`define SC 3'b000


module tx_wr_pkt_to_bram (

    input    trn_clk,
    input    trn_lnk_up_n,

    input       [63:0]      trn_rd,
    input       [7:0]       trn_rrem_n,
    input                   trn_rsof_n,
    input                   trn_reof_n,
    input                   trn_rsrc_rdy_n,
    input                   trn_rsrc_dsc_n,
    input       [6:0]       trn_rbar_hit_n,
    input                   trn_rdst_rdy_n,

    input       [63:0]      huge_page_addr_1,
    input       [63:0]      huge_page_addr_2,
    input       [31:0]      huge_page_qwords_1,
    input       [31:0]      huge_page_qwords_2,
    input                   huge_page_status_1,
    input                   huge_page_status_2,
    output reg              huge_page_free_1,
    output reg              huge_page_free_2,
    input                   interrupts_enabled,

    output reg   [63:0]     huge_page_addr_read_from,
    output reg              read_chunk,
    output reg   [8:0]      qwords_to_rd,
    input                   read_chunk_ack,
    output reg              send_huge_page_rd_completed,
    input                   send_huge_page_rd_completed_ack,
    
    output reg              send_interrupt,
    input                   send_interrupt_ack,

    // Internal memory driver
    output reg  [`BF:0]     wr_addr,
    output reg  [63:0]      wr_data,
    output reg              wr_en,

    input       [`BF:0]     commited_rd_address,             // 156.25 MHz driven
    input                   commited_rd_address_change,      // 156.25 MHz driven
    output reg              wr_addr_updated,                 // to 156.25 MHz
    output reg  [`BF:0]     commited_wr_addr
    );

    wire            reset_n;

    // localparam
    localparam s0  = 15'b000000000000000;
    localparam s1  = 15'b000000000000001;
    localparam s2  = 15'b000000000000010;
    localparam s3  = 15'b000000000000100;
    localparam s4  = 15'b000000000001000;
    localparam s5  = 15'b000000000010000;
    localparam s6  = 15'b000000000100000;
    localparam s7  = 15'b000000001000000;
    localparam s8  = 15'b000000010000000;
    localparam s9  = 15'b000000100000000;
    localparam s10 = 15'b000001000000000;
    localparam s11 = 15'b000010000000000;
    localparam s12 = 15'b000100000000000;
    localparam s13 = 15'b001000000000000;
    localparam s14 = 15'b010000000000000;
    localparam s15 = 15'b100000000000000;

    //-------------------------------------------------------
    // Local 156.25 MHz signal synch
    //-------------------------------------------------------
    reg     [`BF:0] commited_rd_address_reg0;
    reg     [`BF:0] commited_rd_address_reg1;
    reg             commited_rd_address_change_reg0;
    reg             commited_rd_address_change_reg1;
    
    //-------------------------------------------------------
    // Local current_huge_page_addr
    //-------------------------------------------------------
    reg     [63:0]  current_huge_page_addr;
    reg     [31:0]  current_huge_page_qwords;
    reg     [14:0]  give_huge_page_fsm;
    reg     [14:0]  free_huge_page_fsm;
    reg             huge_page_available;

    //-------------------------------------------------------
    // Local trigger_rd_tlp
    //-------------------------------------------------------   
    reg             return_huge_page_to_host;
    reg     [14:0]  trigger_rd_tlp_fsm;
    reg     [`BF:0] diff;
    reg     [8:0]   next_wr_addr;
    reg     [8:0]   look_ahead_next_wr_addr;
    reg     [8:0]   huge_page_qwords_counter;                                   // the width can be less
    reg     [8:0]   look_ahead_huge_page_qwords_counter;
    reg     [63:0]  look_ahead_huge_page_addr_read_from;
    reg     [31:0]  remaining_qwords;
    
    //-------------------------------------------------------
    // Local trigger_interrupts
    //-------------------------------------------------------
    reg     [14:0]  trigger_interrupts_fsm;
    reg     [9:0]   commulative_rd_data;
    reg     [9:0]   commulative_received_data;

    //-------------------------------------------------------
    // Local pulse generation for 156.25 MHz domain
    //-------------------------------------------------------
    reg     [14:0]  pulse_gen_fsm1;

    //-------------------------------------------------------
    // Local completion_tlp & write to bram (wr_to_bram_fsm)
    //-------------------------------------------------------
    reg     [14:0]  wr_to_bram_fsm;
    reg             wr_addr_updated_internal;
    reg     [8:0]   qwords_on_tlp;
    reg             completion_received;
    reg     [31:0]  aux;
    reg     [`BF:0] look_ahead_wr_addr;
    
    assign reset_n = ~trn_lnk_up_n;

    ////////////////////////////////////////////////
    // 156.25 MHz signal synch
    ////////////////////////////////////////////////
    always @( posedge trn_clk or negedge reset_n ) begin

        if (!reset_n ) begin  // reset
            commited_rd_address_reg0 <= 'b0;
            commited_rd_address_reg1 <= 'b0;
            commited_rd_address_change_reg0 <= 1'b0;
            commited_rd_address_change_reg1 <= 1'b0;
        end

        else begin  // not reset
            commited_rd_address_reg0 <= commited_rd_address;

            commited_rd_address_change_reg0 <= commited_rd_address_change;
            commited_rd_address_change_reg1 <= commited_rd_address_change_reg0;

            if (commited_rd_address_change_reg1) begin
                commited_rd_address_reg1 <= commited_rd_address_reg0;
            end

        end     // not reset
    end  //always

    ////////////////////////////////////////////////
    // current_huge_page_addr
    ////////////////////////////////////////////////
    always @( posedge trn_clk or negedge reset_n ) begin

        if (!reset_n ) begin  // reset
            huge_page_free_1 <= 1'b0;
            huge_page_free_2 <= 1'b0;
            huge_page_available <= 1'b0;
            current_huge_page_addr <= 64'b0;
            give_huge_page_fsm <= s0;
            free_huge_page_fsm <= s0;
        end

        else begin  // not reset

            case (free_huge_page_fsm)
                s0 : begin
                    if (return_huge_page_to_host) begin
                        huge_page_free_1 <= 1'b1;
                        free_huge_page_fsm <= s1;
                    end
                end
                s1 : begin
                    huge_page_free_1 <= 1'b0;
                    free_huge_page_fsm <= s2;
                end
                s2 : begin
                    if (return_huge_page_to_host) begin
                        huge_page_free_2 <= 1'b1;
                        free_huge_page_fsm <= s3;
                    end
                end
                s3 : begin
                    huge_page_free_2 <= 1'b0;
                    free_huge_page_fsm <= s0;
                end
            endcase

            case (give_huge_page_fsm)
                s0 : begin
                    if (huge_page_status_1) begin
                        huge_page_available <= 1'b1;
                        current_huge_page_addr <= huge_page_addr_1;
                        current_huge_page_qwords <= huge_page_qwords_1;
                        give_huge_page_fsm <= s1;
                    end
                end

                s1 : begin
                    if (return_huge_page_to_host) begin
                        huge_page_available <= 1'b0;
                        give_huge_page_fsm <= s2;
                    end
                end

                s2 : begin
                    if (huge_page_status_2) begin
                        huge_page_available <= 1'b1;
                        current_huge_page_addr <= huge_page_addr_2;
                        current_huge_page_qwords <= huge_page_qwords_2;
                        give_huge_page_fsm <= s3;
                    end
                end

                s3 : begin
                    if (return_huge_page_to_host) begin
                        huge_page_available <= 1'b0;
                        give_huge_page_fsm <= s0;
                    end
                end
            endcase

        end     // not reset
    end  //always

    ////////////////////////////////////////////////
    // trigger_rd_tlp
    ////////////////////////////////////////////////
    always @( posedge trn_clk or negedge reset_n ) begin

        if (!reset_n ) begin  // reset
            return_huge_page_to_host <= 1'b0;
            read_chunk <= 1'b0;
            send_huge_page_rd_completed <= 1'b0;
            diff <= 'b0;
            next_wr_addr <= 'b0;
            trigger_rd_tlp_fsm <= s0;
        end
        
        else begin  // not reset

            return_huge_page_to_host <= 1'b0;
            diff <= next_wr_addr + (~commited_rd_address_reg1) + 1;
            remaining_qwords <= current_huge_page_qwords + (~huge_page_qwords_counter) + 1;

            case (trigger_rd_tlp_fsm)

                s0 : begin
                    huge_page_addr_read_from <= current_huge_page_addr;
                    huge_page_qwords_counter <= 'b0;
                    if (huge_page_available) begin
                        trigger_rd_tlp_fsm <= s1;
                    end
                end

                s1 : begin
                    look_ahead_next_wr_addr <= next_wr_addr + 'h40;
                    look_ahead_huge_page_addr_read_from <= huge_page_addr_read_from + 'h200;
                    qwords_to_rd <= remaining_qwords[31:6] ? 'h040 : {2'b0, remaining_qwords[5:0]};
                    if (diff < 'h1C0) begin
                        read_chunk <= 1'b1;
                        trigger_rd_tlp_fsm <= s2;
                    end
                end

                s2 : begin
                    next_wr_addr <= look_ahead_next_wr_addr;
                    look_ahead_huge_page_qwords_counter <= huge_page_qwords_counter + qwords_to_rd;
                    if (read_chunk_ack) begin
                        read_chunk <= 1'b0;
                        trigger_rd_tlp_fsm <= s3;
                    end
                end

                s3 : begin
                    huge_page_addr_read_from <= look_ahead_huge_page_addr_read_from;
                    huge_page_qwords_counter <= look_ahead_huge_page_qwords_counter;
                    trigger_rd_tlp_fsm <= s4;
                end

                s4 : begin
                    if (huge_page_qwords_counter < current_huge_page_qwords) begin
                        trigger_rd_tlp_fsm <= s1;
                    end
                    else begin
                        return_huge_page_to_host <= 1'b1;
                        send_huge_page_rd_completed <= 1'b1;
                        trigger_rd_tlp_fsm <= s5;
                    end
                end

                s5 : begin
                    if (send_huge_page_rd_completed_ack) begin
                        send_huge_page_rd_completed <= 1'b0;
                        trigger_rd_tlp_fsm <= s0;
                    end
                end

                default : begin
                    trigger_rd_tlp_fsm <= s0;
                end

            endcase
        end     // not reset
    end  //always

    ////////////////////////////////////////////////
    // trigger_interrupts
    ////////////////////////////////////////////////
    always @( posedge trn_clk or negedge reset_n ) begin

        if (!reset_n ) begin  // reset
            send_interrupt <= 1'b0;
            commulative_rd_data <= 'b0;
            commulative_received_data <= 'b0;
            trigger_interrupts_fsm <= s0;
        end
        
        else begin  // not reset

            if (read_chunk && read_chunk_ack) begin
                commulative_rd_data <= commulative_rd_data + qwords_to_rd;
            end
            if (completion_received) begin
                commulative_received_data <= commulative_received_data + qwords_on_tlp;
            end

            case (trigger_interrupts_fsm)

                s0 : begin
                    if ( (commulative_rd_data != commulative_received_data) && (interrupts_enabled) ) begin
                        trigger_interrupts_fsm <= s1;
                    end
                end

                s1 : begin
                    if (commulative_rd_data == commulative_received_data) begin
                        send_interrupt <= 1'b1;
                        trigger_interrupts_fsm <= s2;
                    end
                end

                s2 : begin
                    if (send_interrupt_ack) begin
                        send_interrupt <= 1'b0;
                        trigger_interrupts_fsm <= s0;
                    end
                end

                default : begin
                    trigger_interrupts_fsm <= s0;
                end

            endcase
        end     // not reset
    end  //always

    ////////////////////////////////////////////////
    // pulse generation for 156.25 MHz domain   must be active for 3 clks in 250 MHz domain
    ////////////////////////////////////////////////
    always @( posedge trn_clk or negedge reset_n ) begin
        
        if (!reset_n ) begin  // reset
            wr_addr_updated <= 1'b0;
            pulse_gen_fsm1 <= s0;
        end
        else begin  // not reset

            case (pulse_gen_fsm1)
                s0 : begin
                    wr_addr_updated <= 1'b0;
                    if (wr_addr_updated_internal) begin
                        wr_addr_updated <= 1'b1;
                        pulse_gen_fsm1 <= s1;
                    end
                end
                s1 : pulse_gen_fsm1 <= s2;
                s2 : pulse_gen_fsm1 <= s0;
            endcase

        end     // not reset
    end  //always


    ////////////////////////////////////////////////
    // completion_tlp & write to bram (wr_to_bram_fsm)
    ////////////////////////////////////////////////
    always @( posedge trn_clk or negedge reset_n ) begin

        if (!reset_n ) begin  // reset
            wr_addr <= 'b0;
            look_ahead_wr_addr <= 'b0;
            commited_wr_addr <= 'b0;
            completion_received <= 1'b0;
            wr_en <= 1'b1;
            wr_to_bram_fsm <= s0;
        end
        
        else begin  // not reset

            wr_addr_updated_internal <= 1'b0;
            wr_addr <= look_ahead_wr_addr;
            wr_en <= 1'b1;
            completion_received <= 1'b0;

            case (wr_to_bram_fsm)

                s0 : begin
                    qwords_on_tlp <= trn_rd[41:33];
                    wr_addr_updated_internal <= 1'b1;
                    commited_wr_addr <= look_ahead_wr_addr;
                    if ( (!trn_rsrc_rdy_n) && (!trn_rsof_n) && (!trn_rdst_rdy_n)) begin
                        if ( (trn_rd[62:56] == `CPL_MEM_RD64_FMT_TYPE) && (trn_rd[15:13] == `SC) ) begin
                            wr_to_bram_fsm <= s1;
                        end
                    end
                end

                s1 : begin
                    if ( (!trn_rsrc_rdy_n) && (!trn_rdst_rdy_n)) begin
                        aux <= trn_rd[31:0];
                        wr_to_bram_fsm <= s2;
                    end
                end

                s2 : begin
                    wr_data <= {trn_rd[39:32], trn_rd[47:40], trn_rd[55:48], trn_rd[63:56], aux[7:0], aux[15:8], aux[23:16], aux[31:24]};
                    if ( (!trn_rsrc_rdy_n) && (!trn_rdst_rdy_n)) begin
                        look_ahead_wr_addr <= look_ahead_wr_addr +1;
                        aux <= trn_rd[31:0];
                        if (!trn_reof_n) begin
                            completion_received <= 1'b1;
                            wr_to_bram_fsm <= s0;
                        end
                    end
                end

                default : begin //other TLPs
                    wr_to_bram_fsm <= s0;
                end

            endcase
        end     // not reset
    end  //always
   

endmodule // tx_wr_pkt_to_bram